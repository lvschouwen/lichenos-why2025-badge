//! PSRAM address space: the MMU pages that put the chip at 0x4800_0000 (cached) and 0x8800_0000 (uncached alias),
//! and the cache maintenance the pattern tests need. Entries as in ESP-IDF v6.1 `ext_mem_defs.h` (SOC_MMU_PSRAM_VALID
//! bit 11, SOC_MMU_ACCESS_PSRAM bit 10, 64 KB pages, 1024 entries) written through the PSRAM controller's MMU window
//! (index register SPIMEM2 + 0x380, content register + 0x37C), as the #609 probe did.
use crate::mmio::write32;
use crate::psram::regs::SPIMEM2;

/// Virtual base of the cached alias (SOC_MMU_PSRAM_VADDR_BASE).
pub const CACHED_BASE: usize = 0x4800_0000;
/// Offset of the uncached alias (SOC_NON_CACHEABLE_OFFSET_PSRAM): the same bytes at CACHED_BASE + 0x4000_0000. Reads and probe
/// diagnostics only: single-word stores through it flip bit 8 on this chip (#620), so PSRAM is written through the cached alias
/// (`cached_addr`) with `cache_writeback_invalidate` where the data must reach the chip.
pub const UNCACHED_OFFSET: usize = 0x4000_0000;
pub const PAGE_BYTES: usize = 0x1_0000;
pub const MMU_ENTRIES: usize = 1024;

const MMU_CONTENT: usize = SPIMEM2 + 0x37C;
const MMU_INDEX: usize = SPIMEM2 + 0x380;
const MMU_VALID: u32 = 1 << 11;
const MMU_ACCESS_PSRAM: u32 = 1 << 10;

/// Physical offsets `[EXCLUDED_START, EXCLUDED_END)` are never handed out (#620): uncached single-word writes of address-like data
/// corrupt bit 8 there on this chip, and the workaround keeps allocators and DMA out of the range.
pub const EXCLUDED_START: usize = 0x54_0000;
pub const EXCLUDED_END: usize = 0x5C_0000;

/// Does `[off, off + len)` touch the excluded range?
pub const fn overlaps_excluded(off: usize, len: usize) -> bool {
    len != 0 && off < EXCLUDED_END && off + len > EXCLUDED_START
}

/// First offset at or after `off` outside the excluded range.
pub const fn next_usable(off: usize) -> usize {
    if off >= EXCLUDED_START && off < EXCLUDED_END {
        EXCLUDED_END
    } else {
        off
    }
}

/// Cached-alias address of a physical PSRAM offset (the only alias PSRAM is written through).
pub const fn cached_addr(off: usize) -> usize {
    CACHED_BASE + off
}

/// MMU content word of a mapping to physical page `ppage` (psram page numbers are below 1024).
pub const fn mmu_word(ppage: u32) -> u32 {
    (ppage & 0x3FF) | MMU_VALID | MMU_ACCESS_PSRAM
}

/// Map virtual pages `0..pages` of the PSRAM window to physical pages `0..pages` (a straight 64 KB-page mapping).
///
/// # Safety
/// Writes the PSRAM MMU: only after `init` reached `Upto::Tuned`; `pages` at most `MMU_ENTRIES`.
pub unsafe fn map_identity(pages: usize) {
    assert!(pages <= MMU_ENTRIES);
    for p in 0..pages {
        write32(MMU_INDEX, p as u32);
        write32(MMU_CONTENT, mmu_word(p as u32));
    }
}

const ROM_CACHE_INVALIDATE_ADDR: usize = 0x4FC0_03E4;
const ROM_CACHE_WRITEBACK_INVALIDATE_ADDR: usize = 0x4FC0_03FC;
/// CACHE_MAP_L1_DCACHE | CACHE_MAP_L2_CACHE (rom/cache.h)
const MAP_DATA: u32 = (1 << 4) | (1 << 5);

const ROM_CACHE_SET_L2_MODE: usize = 0x4FC0_03D4;
const ROM_CACHE_INVALIDATE_ALL: usize = 0x4FC0_0404;
/// CACHE_MAP_L2_CACHE
const MAP_L2: u32 = 1 << 5;

/// Invalidate the whole L2 cache (IDF `cache_hal_init_l2_cache`: `Cache_Invalidate_All(CACHE_MAP_L2_CACHE)`). L2 caches only
/// external memory, so no internal-RAM data is lost. The L2 tags are not defined after a download-mode boot: a stale dirty
/// line would be written back to a random PSRAM address when its set is next used.
///
/// # Safety
/// Calls mask ROM code; discards every dirty L2 line.
pub unsafe fn cache_invalidate_all_l2() -> i32 {
    let f: extern "C" fn(u32) -> i32 = core::mem::transmute(ROM_CACHE_INVALIDATE_ALL);
    f(MAP_L2)
}

/// IDF `cache_hal_init_l2_cache`: `Cache_Set_L2_Cache_Mode(size, ways, line)`. v2 uses 256 KB (`CACHE_SIZE_256K` = 10), 8 ways,
/// 128 B lines (`CACHE_LINE_SIZE_128B` = 4); the ROM state after download mode has 64 B lines. Invalidate L2 afterwards.
///
/// # Safety
/// Calls mask ROM code; reconfigures the L2 cache, so nothing may be cached through it meanwhile.
pub unsafe fn cache_set_l2_mode(size_code: u32, ways: u32, line_code: u32) {
    let f: extern "C" fn(u32, u32, u32) = core::mem::transmute(ROM_CACHE_SET_L2_MODE);
    f(size_code, ways, line_code)
}

/// Drop `[addr, addr + len)` from the data caches without writing it back (the next cached read fetches PSRAM).
/// Returns the ROM's status (0 = ok).
///
/// # Safety
/// Calls mask ROM code; discards dirty lines in the range.
pub unsafe fn cache_invalidate(addr: usize, len: usize) -> i32 {
    let f: extern "C" fn(u32, u32, u32) -> i32 = core::mem::transmute(ROM_CACHE_INVALIDATE_ADDR);
    f(MAP_DATA, addr as u32, len as u32)
}

/// Write dirty lines of `[addr, addr + len)` back to PSRAM, then drop them. Returns the ROM's status (0 = ok).
///
/// # Safety
/// Calls mask ROM code.
pub unsafe fn cache_writeback_invalidate(addr: usize, len: usize) -> i32 {
    let f: extern "C" fn(u32, u32, u32) -> i32 = core::mem::transmute(ROM_CACHE_WRITEBACK_INVALIDATE_ADDR);
    f(MAP_DATA, addr as u32, len as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn excluded_range_maths() {
        assert!(!overlaps_excluded(0, EXCLUDED_START));
        assert!(overlaps_excluded(0, EXCLUDED_START + 1));
        assert!(overlaps_excluded(EXCLUDED_START, 4));
        assert!(overlaps_excluded(EXCLUDED_END - 4, 4));
        assert!(!overlaps_excluded(EXCLUDED_END, 4));
        assert!(!overlaps_excluded(EXCLUDED_START, 0));
        assert!(overlaps_excluded(0x50_0000, 0x20_0000));
        assert_eq!(next_usable(0x10_0000), 0x10_0000);
        assert_eq!(next_usable(EXCLUDED_START), EXCLUDED_END);
        assert_eq!(next_usable(EXCLUDED_END - 1), EXCLUDED_END);
        assert_eq!(next_usable(EXCLUDED_END), EXCLUDED_END);
        assert_eq!(cached_addr(0x1000), 0x4800_1000);
        // the hole is page aligned, so whole 64 KB pages are either in or out
        assert_eq!(EXCLUDED_START % PAGE_BYTES, 0);
        assert_eq!(EXCLUDED_END % PAGE_BYTES, 0);
    }

    #[test]
    fn mmu_words() {
        assert_eq!(mmu_word(0), 0xC00);
        assert_eq!(mmu_word(0x10), 0xC10);
        assert_eq!(mmu_word(511), 0xC00 | 511);
        assert_eq!(mmu_word(0xFFFF), 0xC00 | 0x3FF); // page numbers are 10 bits
    }
}
