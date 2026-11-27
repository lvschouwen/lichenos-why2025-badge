//! ESP32-P4 L2 cache size. L2MEM (768 KB) ends at `L2MEM_END`; the cache carve takes the top of it, so the kernel's
//! NOLOAD memory must end below `L2MEM_END - cache size`. CACHE_L2_CACHE_CACHESIZE_CONF_REG is one-hot: bit n set
//! means 256 << n bytes (bit 0 = 256 B ... bit 10 = 256 KB, the documented reset value, bit 12 = 1 MB).
pub const L2MEM_END: usize = 0x4FFC_0000;

/// Decode the one-hot size register; None when it is not exactly one known bit.
pub fn cache_size_bytes(reg: u32) -> Option<usize> {
    let bits = reg & 0x1FFF;
    if reg != bits || bits.count_ones() != 1 {
        return None;
    }
    Some(256usize << bits.trailing_zeros())
}

/// First address of the cache carve for a given cache size.
pub const fn carve_start(size: usize) -> usize {
    L2MEM_END - size
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    #[test]
    fn decodes_the_reset_value_and_edges() {
        assert_eq!(cache_size_bytes(1 << 10), Some(256 * 1024));
        assert_eq!(cache_size_bytes(1 << 9), Some(128 * 1024));
        assert_eq!(cache_size_bytes(1), Some(256));
        assert_eq!(cache_size_bytes(1 << 12), Some(1024 * 1024));
    }
    #[test]
    fn rejects_anything_not_one_known_bit() {
        for v in [0, 0b11, 1 << 13, 1 << 31, (1 << 10) | (1 << 31)] {
            assert_eq!(cache_size_bytes(v), None, "{v:#x}");
        }
    }
    #[test]
    fn carve_for_the_default_256k_cache() {
        assert_eq!(carve_start(256 * 1024), 0x4FF8_0000);
    }
    proptest! {
        #[test]
        fn any_single_low_bit_round_trips(n in 0u32..13) {
            prop_assert_eq!(cache_size_bytes(1 << n), Some(256usize << n));
        }
    }
}
