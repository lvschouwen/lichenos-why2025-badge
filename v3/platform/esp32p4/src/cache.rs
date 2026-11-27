//! The L2 cache controller as the ROM left it.
use crate::mmio::read32;

const CACHE: usize = 0x3FF1_0000; // DR_REG_CACHE_BASE
const L2_CACHESIZE_CONF: usize = CACHE + 0x278; // one-hot size, see lichen_kcore::l2

/// Raw CACHE_L2_CACHE_CACHESIZE_CONF_REG; decode it with `lichen_kcore::l2::cache_size_bytes`.
pub fn l2_cachesize_reg() -> u32 {
    // SAFETY: a cache controller configuration register, a read without side effects.
    unsafe { read32(L2_CACHESIZE_CONF) }
}
