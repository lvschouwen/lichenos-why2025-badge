//! PSRAM in the kernel (#621): bring-up, the kernel-only MMU map, and app memory from `lichen_kcore::palloc`.
//!
//! - **Clock:** PSRAM at 120 MHz, brought up after the CPU went to 360 MHz (#655, bringup `clock`): IDF wants CPU >=
//!   PSRAM MHz (the MSPI write FIFO can overflow otherwise, #620). The #621 stopgap that failed every zeroing on the
//!   controller's sticky FIFO error flags is gone with that; the `psram.mspi` row still reads them after the rows.
//!   The bit-8 flips #620 found are uncached single-word stores, which this module never does.
//! - **MMU:** `map_identity` puts physical page n at virtual page n of the window, once: a physical page has exactly
//!   one cached address and the hardware's uncached alias of it. The MMU registers are SPIMEM2 MMIO, which no U view
//!   grants. The excluded range (#620) stays unmapped.
//! - **U access:** only through `grant(owner, handle)`, which looks the live block up in the allocator: a stale or
//!   another owner's handle gets nothing. The region is the block's cached alias, refused if `palloc::u_region_ok`
//!   refuses it (the uncached alias, the hole, W+X).
//! - **Reclaim:** `release_owner` refuses while this hart's loaded view still reaches one of the owner's blocks.
//! - **Metadata:** the allocator is a zero-initialised static, so it lives in bss in L2MEM. Nothing in-band in PSRAM.
//! - **Single context:** core 0 only, enforced (`with` halts on any other hart); never from a trap handler (the
//!   bring-up and, later, the loader and the reaper; `end_proc` runs in a trap and must leave the reaping to core 0).
//! - **Core 1:** the exec zeroing drops both cores' ICaches, but `fence.i` runs on core 0 only. Core 1 has no PSRAM
//!   PMA entry yet (smp.rs); once it runs PSRAM code it must `fence.i` on every switch into a process whose exec block
//!   was loaded or reused since that core last ran it.
use crate::global::Global;
use crate::kprintln;
use crate::platform::{self, hw};
use hw::psram::{self, map};
use lichen_kcore::palloc::{self, Block, Error, Handle, Kind, Palloc, Scrub, UNCACHED_BASE};
use lichen_kcore::pmp::Region;

const PAGES: usize = map::MAPPED_BYTES / map::PAGE_BYTES;
pub use palloc::{DATA_ARENA, EXEC_ARENA};

const _: () = assert!(palloc::CACHED_BASE == map::CACHED_BASE && palloc::PSRAM_BYTES == map::MAPPED_BYTES);
const _: () = assert!(palloc::HOLE_START == map::EXCLUDED_START && palloc::HOLE_END == map::EXCLUDED_END);

/// SPIMEM2 `mem_int_raw` (probe-psram `regs`): bit 26 RX overflow, bit 27 TX underflow. Never set in #620's runs.
const MSPI_INT_RAW: usize = hw::psram::regs::SPIMEM2 + 0xC8;
pub const MSPI_FIFO_ERRORS: u32 = (1 << 26) | (1 << 27);

static ALLOC: Global<Palloc> = Global::new(Palloc::EMPTY);

/// The controller's FIFO error flags (raw, sticky).
pub fn mspi_errors() -> u32 {
    // SAFETY: a read of a PSRAM controller status register, no side effects.
    unsafe { hw::mmio::read32(MSPI_INT_RAW) & MSPI_FIFO_ERRORS }
}

/// Zeroing through the cached alias, then written back and dropped from the data caches (`map::sync` orders the stores
/// before the operation with `fence iorw, iorw`): the chip holds zeros and no cached line holds the old data. Exec
/// blocks are also dropped from both cores' L1 ICache (`sync_icache`: ROM `Cache_Invalidate_Addr` with the ICache
/// map, then `fence.i`): the next owner must not run the last owner's code. (The uncached alias is never written:
/// #620's bit-8 flips.)
struct CachedZero;
impl Scrub for CachedZero {
    fn zero(&mut self, off: usize, len: usize, kind: Kind) -> bool {
        let Some(a) = map::cached_range(off, len) else { return false };
        // SAFETY: [off, off + len) is mapped PSRAM inside an arena (the allocator only names its own pages). Callers
        // are `with` (core 0, enforced) and `release_owner` has checked no view of this hart reaches the owner's
        // blocks; a fresh grant's pages belong to no one yet.
        unsafe {
            core::ptr::write_bytes(a as *mut u8, 0, len);
            map::cache_writeback_invalidate(a, len) == 0 && (kind == Kind::Data || hw::cache::sync_icache(a, len) == 0)
        }
    }
}

/// Bring PSRAM up at 120 MHz and map it. Returns the init summary, or why it failed.
pub fn up() -> Result<psram::Info, &'static str> {
    on_core0();
    // SAFETY: the one caller, core 0 during bring-up; nothing else uses PSRAM, MPLL or the MSPI pins.
    let info =
        unsafe { psram::init(psram::Upto::Tuned, psram::TuneOverride::default(), &mut |a| kprintln!("psram: {a}")) }
            .map_err(|_| "init")?;
    // SAFETY: init reached Tuned. The L2 tags are undefined after a download-mode boot: invalidate before the first map.
    let rc = unsafe { map::cache_invalidate_all_l2() | map::map_identity(PAGES) };
    if rc != 0 {
        return Err("map");
    }
    with(|a| a.configure(EXEC_ARENA, DATA_ARENA)).map_err(|_| "arenas")?;
    Ok(info)
}

/// Halt unless on core 0: the allocator's `Global` is unlocked and the cache sync registers take one caller.
fn on_core0() {
    if crate::smp::hartid() != 0 {
        platform::enter_fatal();
        kprintln!("KERNEL: PSRAM allocator used off core 0: halting");
        platform::exit(4);
    }
}

fn with<R>(f: impl FnOnce(&mut Palloc) -> R) -> R {
    on_core0();
    // SAFETY: core 0 (checked above) and never from a trap handler (module doc), so this is the only live reference.
    f(unsafe { ALLOC.get() })
}

pub fn set_quota(owner: usize, bytes: usize) -> Result<(), Error> {
    with(|a| a.set_quota(owner, bytes))
}
pub fn alloc(owner: usize, kind: Kind, len: usize) -> Result<(Handle, Block), Error> {
    with(|a| a.alloc(owner, kind, len, &mut CachedZero))
}
pub fn usage(owner: usize) -> usize {
    with(|a| a.usage(owner))
}
fn block(owner: usize, h: Handle) -> Option<Block> {
    with(|a| a.block(owner, h))
}

/// The U region of `owner`'s live block `h`: its cached alias. None for a stale or foreign handle, or a region
/// `u_region_ok` refuses.
pub fn grant(owner: usize, h: Handle) -> Option<Region> {
    let r = palloc::u_region(&block(owner, h)?);
    palloc::u_region_ok(&r).then_some(r)
}

/// Copy `code` into `owner`'s exec block `h` and make it visible to instruction fetch (both ICaches, `fence.i` on
/// core 0). False for a stale or foreign handle, a data block, code that does not fit, or a failed cache operation
/// (the code must not run then).
pub fn load_code(owner: usize, h: Handle, code: &[u32]) -> bool {
    let Some(b) = block(owner, h).filter(|b| b.kind == Kind::Exec && code.len() * 4 <= b.len) else { return false };
    let Some(a) = map::cached_range(b.off, b.len) else { return false };
    // SAFETY: the owner's live exec block (looked up above), mapped; on core 0 with no U running.
    unsafe {
        core::ptr::copy_nonoverlapping(code.as_ptr(), a as *mut u32, code.len());
        hw::cache::sync_icache(a, code.len() * 4) == 0
    }
}

/// Write the cached lines of `[off, off + len)` back to the chip (they stay cached). False if the range is not an
/// arena's or the sync failed.
pub fn writeback(off: usize, len: usize) -> bool {
    on_core0();
    // SAFETY: core 0 (checked), the one user of the cache sync registers.
    map::cached_range(off, len).is_some_and(|a| unsafe { map::cache_writeback(a, len) } == 0)
}

/// Does this hart's loaded view reach `b` through either alias?
fn visible(b: &Block) -> bool {
    let c = palloc::CACHED_BASE + b.off;
    let u = UNCACHED_BASE + b.off;
    crate::pmp::shadow_grants_any(c, c + b.len) || crate::pmp::shadow_grants_any(u, u + b.len)
}

/// Every block of `owner` back, zeroed: the hook for a process's end, run by core 0. None (nothing freed) while this
/// hart's loaded PMP view still reaches one of them: load another view first. Some(false): freed, but a zeroing
/// failed, so those pages stay dirty and are zeroed before the next grant.
pub fn release_owner(owner: usize) -> Option<bool> {
    with(|a| {
        if a.blocks().any(|b| b.owner == owner && visible(&b)) {
            return None;
        }
        Some(a.release_owner(owner, &mut CachedZero))
    })
}

/// M's cached view of physical offset `off` (one word).
pub fn read_cached(off: usize) -> u32 {
    assert!(off & 3 == 0, "unaligned PSRAM word");
    let a = map::cached_addr(off).unwrap_or_else(|| platform::exit(4));
    // SAFETY: `a` is the mapped cached alias of `off` (cached_addr refuses the hole and the end), 4-byte aligned.
    unsafe { core::ptr::read_volatile(a as *const u32) }
}

/// The chip's content at `off`, past every cache (diagnostics: a cached-only check passes while the chip is stale).
pub fn read_uncached(off: usize) -> u32 {
    // SAFETY: `off` is in a mapped page (callers pass arena offsets) and 4-byte aligned (asserted inside).
    unsafe { map::uncached_read32(off) }
}
