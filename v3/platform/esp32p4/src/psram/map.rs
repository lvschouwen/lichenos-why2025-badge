//! PSRAM address space: the MMU pages that put the chip at 0x4800_0000 (cached) and 0x8800_0000 (uncached alias),
//! and the cache maintenance (ported from IDF's patched write-back, see `sync`). Entries as in ESP-IDF v6.1 `ext_mem_defs.h` (SOC_MMU_PSRAM_VALID
//! bit 11, SOC_MMU_ACCESS_PSRAM bit 10, 64 KB pages, 1024 entries) written through the PSRAM controller's MMU window
//! (index register SPIMEM2 + 0x380, content register + 0x37C), as the #609 probe did.
use crate::mmio::{read32, write32};
use crate::psram::regs::SPIMEM2;

/// Virtual base of the cached alias (SOC_MMU_PSRAM_VADDR_BASE).
pub const CACHED_BASE: usize = 0x4800_0000;
/// Offset of the uncached alias (SOC_NON_CACHEABLE_OFFSET_PSRAM): the same bytes at CACHED_BASE + 0x4000_0000. Not public:
/// single-word stores through it flip bit 8 on this chip (#620), so PSRAM is written through the cached alias (`cached_addr`) with
/// `cache_writeback_invalidate` where the data must reach the chip. Reads for diagnostics go through `uncached_read32`.
pub(crate) const UNCACHED_OFFSET: usize = 0x4000_0000;
pub const PAGE_BYTES: usize = 0x1_0000;
pub const MMU_ENTRIES: usize = 1024;
/// What v3 maps: the whole 32 MB chip.
pub const MAPPED_BYTES: usize = 32 << 20;

const MMU_CONTENT: usize = SPIMEM2 + 0x37C;
const MMU_INDEX: usize = SPIMEM2 + 0x380;
const MMU_VALID: u32 = 1 << 11;
const MMU_ACCESS_PSRAM: u32 = 1 << 10;

/// Physical offsets `[EXCLUDED_START, EXCLUDED_END)` are never handed out (#620): uncached single-word writes of address-like data
/// corrupt bit 8 there on this chip, and the workaround keeps allocators and DMA out of the range. Its MMU pages stay unmapped.
pub const EXCLUDED_START: usize = 0x54_0000;
pub const EXCLUDED_END: usize = 0x5C_0000;

/// Does `[off, off + len)` touch the excluded range? A range that wraps the address space counts as touching.
pub const fn overlaps_excluded(off: usize, len: usize) -> bool {
    match off.checked_add(len) {
        None => true,
        Some(end) => len != 0 && off < EXCLUDED_END && end > EXCLUDED_START,
    }
}

/// The first offset at or after `off` where `[o, o + len)` avoids the excluded range and fits in `MAPPED_BYTES`; `None` if there
/// is none (or the arithmetic overflows).
pub const fn next_usable(off: usize, len: usize) -> Option<usize> {
    let o = if overlaps_excluded(off, len) {
        if off < EXCLUDED_END {
            EXCLUDED_END
        } else {
            return None; // overflowing or beyond the chip: nothing at or after `off`
        }
    } else {
        off
    };
    match o.checked_add(len) {
        Some(end) if end <= MAPPED_BYTES && !overlaps_excluded(o, len) => Some(o),
        _ => None,
    }
}

/// Cached-alias address of a physical PSRAM offset (the only alias PSRAM is written through); `None` for the excluded range and
/// for offsets outside the mapped chip.
pub const fn cached_addr(off: usize) -> Option<usize> {
    cached_range(off, 1)
}

/// Cached-alias address of the start of `[off, off + len)`; `None` if the range touches the excluded range or leaves the chip.
pub const fn cached_range(off: usize, len: usize) -> Option<usize> {
    if len == 0 || overlaps_excluded(off, len) {
        return None;
    }
    match off.checked_add(len) {
        Some(end) if end <= MAPPED_BYTES => Some(CACHED_BASE + off),
        _ => None,
    }
}

/// Read one word through the uncached alias (the memory itself, no cache): diagnostics only.
///
/// # Safety
/// The page of `off` must be mapped (`map_identity`, or `map_identity_with_hole` for the excluded range); `off` 4-byte aligned.
pub unsafe fn uncached_read32(off: usize) -> u32 {
    assert!(off < MAPPED_BYTES && off & 3 == 0);
    core::ptr::read_volatile((CACHED_BASE + UNCACHED_OFFSET + off) as *const u32)
}

/// MMU content word of a mapping to physical page `ppage` (psram page numbers are below 1024).
pub const fn mmu_word(ppage: u32) -> u32 {
    (ppage & 0x3FF) | MMU_VALID | MMU_ACCESS_PSRAM
}

unsafe fn map_pages(pages: usize, with_hole: bool) {
    assert!(pages <= MMU_ENTRIES);
    for p in 0..pages {
        let hole = overlaps_excluded(p * PAGE_BYTES, PAGE_BYTES);
        write32(MMU_INDEX, p as u32);
        write32(MMU_CONTENT, if hole && !with_hole { 0 } else { mmu_word(p as u32) });
    }
}

/// Map virtual pages `0..pages` of the PSRAM window to physical pages `0..pages` (64 KB pages), leaving the excluded range's pages
/// unmapped (their MMU entries are written invalid).
///
/// # Safety
/// Writes the PSRAM MMU: only after `init` reached `Upto::Tuned`; `pages` at most `MMU_ENTRIES`.
pub unsafe fn map_identity(pages: usize) {
    map_pages(pages, false)
}

/// As `map_identity` but also maps the excluded range: for the diagnostics that test the hole and nothing else.
///
/// # Safety
/// As `map_identity`; nothing but diagnostics may use the hole.
pub unsafe fn map_identity_with_hole(pages: usize) {
    map_pages(pages, true)
}

// ---- cache maintenance ----------------------------------------------------------------------------------------------------
// CACHE_SYNC registers of the cache controller (soc/cache_reg.h, hw_ver1)
const CACHE: usize = 0x3FF1_0000;
const SYNC_CTRL: usize = CACHE + 0x98;
const SYNC_MAP: usize = CACHE + 0x9C;
const SYNC_ADDR: usize = CACHE + 0xA0;
const SYNC_SIZE: usize = CACHE + 0xA4;
const L1D_BLOCKSIZE: usize = CACHE + 0x1C;
const L2_BLOCKSIZE: usize = CACHE + 0x27C;
const WRITEBACK_ENA: u32 = 0x4;
const WRITEBACK_INVALIDATE_ENA: u32 = 0x8;
const SYNC_DONE: u32 = 0x10;

/// Line size in bytes from a `*_BLOCKSIZE_CONF` register (one-hot, bit 2 = 32 B, bit 3 = 64 B, bit 4 = 128 B).
pub const fn line_bytes_from_reg(reg: u32) -> usize {
    if reg == 0 {
        64
    } else {
        1usize << (reg.trailing_zeros() + 3)
    }
}

/// IDF's patched write-back range (`esp_rom/patches/esp_rom_cache_writeback_esp32p4_esp32s31.c`): address down and size up to the
/// larger of the L1 and L2 line.
pub const fn align_to_line(addr: u32, size: u32, line: u32) -> (u32, u32) {
    let plus = addr & (line - 1);
    let size = size + plus;
    (addr - plus, (size + line - 1) & !(line - 1))
}

unsafe fn sync(ctrl_op: u32, addr: usize, len: usize) {
    let l1 = line_bytes_from_reg(read32(L1D_BLOCKSIZE)) as u32;
    let l2 = line_bytes_from_reg(read32(L2_BLOCKSIZE)) as u32;
    let (a, s) = align_to_line(addr as u32, len as u32, l1.max(l2));
    write32(SYNC_MAP, MAP_DATA);
    write32(SYNC_ADDR, a);
    write32(SYNC_SIZE, s);
    // IDF issues the operation twice, "to avoid sync loss": the ROM versions do it once and are replaced in IDF on the P4
    for _ in 0..2 {
        write32(SYNC_CTRL, ctrl_op);
        while read32(SYNC_CTRL) & SYNC_DONE == 0 {}
    }
}

/// Write dirty lines of `[addr, addr + len)` back to PSRAM, keeping them cached (IDF's patched `Cache_WriteBack_Addr`).
///
/// # Safety
/// Cache controller registers; one caller.
pub unsafe fn cache_writeback(addr: usize, len: usize) -> i32 {
    sync(WRITEBACK_ENA, addr, len);
    0
}

/// Write dirty lines back, then drop the range (IDF's patched `Cache_WriteBack_Invalidate_Addr`). Returns 0.
///
/// # Safety
/// Cache controller registers; one caller.
pub unsafe fn cache_writeback_invalidate(addr: usize, len: usize) -> i32 {
    sync(WRITEBACK_INVALIDATE_ENA, addr, len);
    0
}

const ROM_CACHE_INVALIDATE_ADDR: usize = 0x4FC0_03E4;
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
        // wrap-around counts as touching instead of overflowing
        assert!(overlaps_excluded(usize::MAX - 3, 8));
        assert!(overlaps_excluded(usize::MAX, usize::MAX));
        // the hole is page aligned, so whole 64 KB pages are either in or out
        assert_eq!(EXCLUDED_START & (PAGE_BYTES - 1), 0);
        assert_eq!(EXCLUDED_END & (PAGE_BYTES - 1), 0);
    }

    #[test]
    fn next_usable_skips_the_hole_and_respects_the_end() {
        assert_eq!(next_usable(0x10_0000, 0x1000), Some(0x10_0000));
        assert_eq!(next_usable(EXCLUDED_START - 0x1000, 0x1000), Some(EXCLUDED_START - 0x1000));
        // a range that would run into the hole starts after it
        assert_eq!(next_usable(EXCLUDED_START - 0x1000, 0x2000), Some(EXCLUDED_END));
        assert_eq!(next_usable(EXCLUDED_START, 4), Some(EXCLUDED_END));
        assert_eq!(next_usable(EXCLUDED_END - 1, 4), Some(EXCLUDED_END));
        assert_eq!(next_usable(EXCLUDED_END, 4), Some(EXCLUDED_END));
        // the end of the chip and overflow
        assert_eq!(next_usable(MAPPED_BYTES - 0x1000, 0x1000), Some(MAPPED_BYTES - 0x1000));
        assert_eq!(next_usable(MAPPED_BYTES - 0x1000, 0x2000), None);
        assert_eq!(next_usable(usize::MAX - 1, 8), None);
        assert_eq!(next_usable(0, usize::MAX), None);
    }

    #[test]
    fn cached_addresses_refuse_the_hole_and_the_end() {
        assert_eq!(cached_addr(0x1000), Some(0x4800_1000));
        assert_eq!(cached_addr(EXCLUDED_START), None);
        assert_eq!(cached_addr(EXCLUDED_END - 1), None);
        assert_eq!(cached_addr(EXCLUDED_END), Some(CACHED_BASE + EXCLUDED_END));
        assert_eq!(cached_addr(MAPPED_BYTES), None);
        assert_eq!(cached_range(0, MAPPED_BYTES), None); // touches the hole
        assert_eq!(cached_range(EXCLUDED_END, MAPPED_BYTES - EXCLUDED_END), Some(CACHED_BASE + EXCLUDED_END));
        assert_eq!(cached_range(EXCLUDED_END, MAPPED_BYTES - EXCLUDED_END + 1), None);
        assert_eq!(cached_range(0, 0), None);
        assert_eq!(cached_range(usize::MAX, 2), None);
    }

    #[test]
    fn cache_line_maths_follows_the_idf_patch() {
        assert_eq!(line_bytes_from_reg(0x08), 64);
        assert_eq!(line_bytes_from_reg(0x10), 128);
        assert_eq!(line_bytes_from_reg(0x04), 32);
        // addr down to the line, size up: 0x48000010 + 0x30 with 128 B lines is the whole first line
        assert_eq!(align_to_line(0x4800_0010, 0x30, 128), (0x4800_0000, 128));
        assert_eq!(align_to_line(0x4800_0000, 0x1_0000, 128), (0x4800_0000, 0x1_0000));
        assert_eq!(align_to_line(0x4800_007F, 2, 64), (0x4800_0040, 128)); // straddles two lines
        assert_eq!(align_to_line(0x4800_0040, 1, 64), (0x4800_0040, 64));
    }

    #[test]
    fn mmu_words() {
        assert_eq!(mmu_word(0), 0xC00);
        assert_eq!(mmu_word(0x10), 0xC10);
        assert_eq!(mmu_word(511), 0xC00 | 511);
        assert_eq!(mmu_word(0xFFFF), 0xC00 | 0x3FF); // page numbers are 10 bits
    }
}
