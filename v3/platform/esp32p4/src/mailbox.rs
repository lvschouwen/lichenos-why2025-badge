//! Core 0 <-> core 1 mailbox, the hardware side (#612 checkpoint 3). The queue itself is `lichen_kcore::mailbox::Spsc`
//! (pure, host-tested; its doc has the ordering and cache argument). What is P4-specific lives here.
//!
//! - Placement: the queues are kernel statics in cached L2MEM (0x4FF..). The P4 has one L1 DCache for both HP cores
//!   (esp_rom cache.h: `CACHE_MAP_L1_DCACHE` next to the per-core `CACHE_MAP_L1_ICACHE_0/1`), so the two harts share
//!   every cached line and need fences only, no write-back or invalidate.
//! - Never through the uncached alias: `UNCACHED_OFFSET` above the cached address bypasses that L1 (write-back, so the
//!   alias can show stale memory). The bench reads it once, as evidence of where the data lives, and nothing else does.
//! - Instructions are per core: code written by one hart and run by the other needs the other hart's ICache
//!   invalidated (#618 loads apps; not needed here, the image is loaded before core 1 leaves reset).
//! - Polled: there is no doorbell interrupt yet. #619 may add the HP_SYSTEM CPU_INTR_FROM_CPU lines for a wake.
use crate::mmio::read32;

/// The uncached alias of L2MEM: 0x4FF0_0000 + this = 0x8FF0_0000.
pub const UNCACHED_OFFSET: usize = 0x4000_0000;

/// Read a word of cached L2MEM through the uncached alias (bench evidence only).
///
/// # Safety
/// `cached` must be a 4-byte aligned address in L2MEM (0x4FF0_0000..0x4FFC_0000).
pub unsafe fn read_uncached(cached: usize) -> u32 {
    read32(cached + UNCACHED_OFFSET)
}
