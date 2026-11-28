//! The L2 cache controller as the ROM left it.
use crate::mmio::read32;

const CACHE: usize = 0x3FF1_0000; // DR_REG_CACHE_BASE
const L2_CACHESIZE_CONF: usize = CACHE + 0x278; // one-hot size, see lichen_kcore::l2

/// Raw CACHE_L2_CACHE_CACHESIZE_CONF_REG; decode it with `lichen_kcore::l2::cache_size_bytes`.
pub fn l2_cachesize_reg() -> u32 {
    // SAFETY: a cache controller configuration register, a read without side effects.
    unsafe { read32(L2_CACHESIZE_CONF) }
}

/// ROM `Cache_Invalidate_Addr(map, addr, size)` (esp32p4.rom.ld); IDF calls the ROM version for the ICache too
/// (`cache_ll_l1_invalidate_icache_addr`), unlike the write-back, which IDF patches (see `psram::map::cache_writeback`).
#[cfg(target_arch = "riscv32")]
const ROM_CACHE_INVALIDATE_ADDR: usize = 0x4FC0_03E4;
/// CACHE_MAP_L1_ICACHE_0 | CACHE_MAP_L1_ICACHE_1 (rom/cache.h): both cores' instruction caches.
#[cfg(target_arch = "riscv32")]
const MAP_L1_ICACHE: u32 = 0b11;

/// Make code just written to `[addr, addr + len)` of L2MEM visible to instruction fetch. L2MEM sits behind the shared
/// write-back L1 DCache and a per-core L1 ICache (SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE), so `fence.i` alone is not
/// enough: the dirty lines go back to L2MEM first, then both ICaches drop the range. Returns 0, or nonzero when a
/// cache operation failed (the code must not run then).
///
/// # Safety
/// Cache controller registers and mask ROM code; one caller at a time. The range must be L2MEM that no core is
/// executing from.
#[cfg(target_arch = "riscv32")]
pub unsafe fn sync_icache(addr: usize, len: usize) -> i32 {
    let wb = crate::psram::map::cache_writeback(addr, len);
    if wb != 0 {
        return wb;
    }
    let inv: extern "C" fn(u32, u32, u32) -> i32 = core::mem::transmute(ROM_CACHE_INVALIDATE_ADDR);
    let rc = inv(MAP_L1_ICACHE, addr as u32, len as u32);
    core::arch::asm!("fence", "fence.i", options(nostack));
    rc
}
