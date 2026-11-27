//! Test patterns for memory checks: pure functions of an index, so a checker can regenerate any word it expects
//! without keeping a copy (PSRAM is 32 MB, internal RAM is not).

/// Walking-ones pattern `n` (0..32): bit `n` set.
pub const fn walking_one(n: u32) -> u32 {
    1u32 << (n & 31)
}
/// Walking-zeros pattern `n`: every bit but `n` set.
pub const fn walking_zero(n: u32) -> u32 {
    !walking_one(n)
}
/// Address-in-address: every word holds its own byte address (a stuck or aliased address line shows as a wrong word).
/// `inverted` stores the complement, so a pass writes both polarities over every cell.
pub const fn addr_word(byte_addr: u32, inverted: bool) -> u32 {
    if inverted {
        !byte_addr
    } else {
        byte_addr
    }
}
/// Stateless pseudo-random word for index `i` under `seed` (splitmix32-style finaliser): any word is recomputable.
pub const fn prng_word(seed: u32, i: u32) -> u32 {
    let mut x = seed.wrapping_add(i.wrapping_mul(0x9E37_79B9));
    x = (x ^ (x >> 16)).wrapping_mul(0x85EB_CA6B);
    x = (x ^ (x >> 13)).wrapping_mul(0xC2B2_AE35);
    x ^ (x >> 16)
}
/// Bits that differ between what was expected and what was read, as a count (bit-flip statistics for #569).
pub const fn flipped_bits(expected: u32, got: u32) -> u32 {
    (expected ^ got).count_ones()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn walking_patterns_cover_every_bit_once() {
        let (mut ones, mut zeros) = (0u32, 0u32);
        for n in 0..32 {
            assert_eq!(walking_one(n).count_ones(), 1);
            assert_eq!(walking_zero(n).count_ones(), 31);
            assert_eq!(walking_one(n) ^ walking_zero(n), u32::MAX);
            ones |= walking_one(n);
            zeros |= !walking_zero(n);
        }
        assert_eq!((ones, zeros), (u32::MAX, u32::MAX));
    }

    #[test]
    fn address_pattern_distinguishes_every_word_and_polarity() {
        assert_eq!(addr_word(0x1234_5678, false), 0x1234_5678);
        assert_eq!(addr_word(0x1234_5678, true), !0x1234_5678);
        assert_ne!(addr_word(0x1000, false), addr_word(0x1004, false));
    }

    #[test]
    fn prng_is_stable_and_spreads_bits() {
        assert_eq!(prng_word(1, 0), prng_word(1, 0));
        assert_ne!(prng_word(1, 0), prng_word(2, 0));
        assert_ne!(prng_word(1, 0), prng_word(1, 1));
        // each bit position is set in about half of 4096 words (a stuck bit in the generator would show here)
        for bit in 0..32 {
            let n = (0..4096u32).filter(|&i| prng_word(0xC0FF_EE11, i) >> bit & 1 == 1).count();
            assert!((1800..2300).contains(&n), "bit {bit}: {n} of 4096");
        }
    }

    #[test]
    fn flipped_bit_count() {
        assert_eq!(flipped_bits(0, 0x100), 1); // the #86 failure: bit 8 0 -> 1
        assert_eq!(flipped_bits(0xFFFF_FFFF, 0), 32);
        assert_eq!(flipped_bits(5, 5), 0);
    }
}
