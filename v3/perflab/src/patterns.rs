//! The integrity passes (#648, in the style of the #86 stress): dense (every word is written), complementary (a pass and its
//! inverse, so every cell is driven to both levels) and multi-pass. The v2 flips were 0 -> 1 on bit 8 of the word at 0x400 mod 0x800
//! in physical 0x55xxxx-0x62xxxx, so the all-zero pass and the watch class are counted separately.
use lichen_platform_esp32p4::psram::patterns::{addr_word, prng_word};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pattern {
    /// The word's own byte offset.
    Addr,
    /// Its complement.
    AddrInv,
    Rand(u32),
    RandInv(u32),
    Aa,
    Mask55,
    Ones,
    Zeros,
}

/// One round over the chip: every pattern and its opposite.
pub const PASSES: [Pattern; 8] = [
    Pattern::Addr,
    Pattern::AddrInv,
    Pattern::Rand(0xC0FF_EE11),
    Pattern::RandInv(0xC0FF_EE11),
    Pattern::Aa,
    Pattern::Mask55,
    Pattern::Zeros,
    Pattern::Ones,
];

impl Pattern {
    pub const fn name(self) -> &'static str {
        match self {
            Pattern::Addr => "addr",
            Pattern::AddrInv => "addr_inv",
            Pattern::Rand(_) => "rand",
            Pattern::RandInv(_) => "rand_inv",
            Pattern::Aa => "aa",
            Pattern::Mask55 => "55",
            Pattern::Ones => "ones",
            Pattern::Zeros => "zeros",
        }
    }
    /// The complementary pattern: every word is the bitwise inverse. A pass that writes this after `self` changes every bit of the
    /// chip, so a write that is dropped cannot read back as the expected data.
    pub const fn opposite(self) -> Pattern {
        match self {
            Pattern::Addr => Pattern::AddrInv,
            Pattern::AddrInv => Pattern::Addr,
            Pattern::Rand(s) => Pattern::RandInv(s),
            Pattern::RandInv(s) => Pattern::Rand(s),
            Pattern::Aa => Pattern::Mask55,
            Pattern::Mask55 => Pattern::Aa,
            Pattern::Ones => Pattern::Zeros,
            Pattern::Zeros => Pattern::Ones,
        }
    }
    /// The word that belongs at byte offset `off` (4-byte aligned) of the chip.
    pub const fn word(self, off: u32) -> u32 {
        match self {
            Pattern::Addr => addr_word(off, false),
            Pattern::AddrInv => addr_word(off, true),
            Pattern::Rand(s) => prng_word(s, off / 4),
            Pattern::RandInv(s) => !prng_word(s, off / 4),
            Pattern::Aa => 0xAAAA_AAAA,
            Pattern::Mask55 => 0x5555_5555,
            Pattern::Ones => u32::MAX,
            Pattern::Zeros => 0,
        }
    }
}

/// Physical window where v2's flips sat (#86: 0x55_8C01 to 0x62_E401): the class that matters is checked inside it.
pub const WATCH_START: u32 = 0x54_0000;
pub const WATCH_END: u32 = 0x64_0000;
/// The word at 0x400 mod 0x800 inside the window.
pub const fn in_watch_class(off: u32) -> bool {
    off & 0x7FF == 0x400 && off >= WATCH_START && off < WATCH_END
}

pub const FIRST_KEPT: usize = 6;

/// Verdict counters of one pass over a range.
pub struct Tally {
    pub words: u64,
    pub bad: u64,
    /// Bad words in the watch class.
    pub watch: u64,
    /// Per bit: flipped 0 -> 1 and 1 -> 0.
    pub up: [u32; 32],
    pub down: [u32; 32],
    /// The first bad words: (byte offset, expected, got).
    pub first: [(u32, u32, u32); FIRST_KEPT],
}
impl Tally {
    pub const fn new() -> Self {
        Tally { words: 0, bad: 0, watch: 0, up: [0; 32], down: [0; 32], first: [(0, 0, 0); FIRST_KEPT] }
    }
    pub fn check(&mut self, off: u32, expected: u32, got: u32) {
        self.words += 1;
        if expected == got {
            return;
        }
        if (self.bad as usize) < FIRST_KEPT {
            self.first[self.bad as usize] = (off, expected, got);
        }
        self.bad += 1;
        self.watch += in_watch_class(off) as u64;
        let d = expected ^ got;
        for b in 0..32 {
            if d >> b & 1 == 1 {
                if got >> b & 1 == 1 {
                    self.up[b] += 1;
                } else {
                    self.down[b] += 1;
                }
            }
        }
    }
    pub fn clean(&self) -> bool {
        self.bad == 0
    }
    /// Fold another tally in (the per-chunk tallies of one pass).
    pub fn merge(&mut self, o: &Tally) {
        for (i, f) in o.first.iter().take(o.bad.min(FIRST_KEPT as u64) as usize).enumerate() {
            let at = self.bad as usize + i;
            if at < FIRST_KEPT {
                self.first[at] = *f;
            }
        }
        self.words += o.words;
        self.bad += o.bad;
        self.watch += o.watch;
        for b in 0..32 {
            self.up[b] += o.up[b];
            self.down[b] += o.down[b];
        }
    }
}
impl Default for Tally {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_pass_has_its_opposite() {
        // each cell is driven to both levels over one round: the passes pair up as complements
        for off in [0u32, 4, 0x400, 0x55_8C00, 0x1FF_FFFC] {
            assert_eq!(Pattern::Addr.word(off), !Pattern::AddrInv.word(off));
            assert_eq!(Pattern::Rand(9).word(off), !Pattern::RandInv(9).word(off));
            assert_eq!(Pattern::Aa.word(off), !Pattern::Mask55.word(off));
            assert_eq!(Pattern::Zeros.word(off), !Pattern::Ones.word(off));
        }
        let names: std::collections::BTreeSet<_> = PASSES.iter().map(|p| p.name()).collect();
        assert_eq!(names.len(), PASSES.len());
    }

    #[test]
    fn the_opposite_changes_every_bit_of_every_word() {
        for p in PASSES {
            assert_eq!(p.opposite().opposite(), p);
            for off in [0u32, 4, 0x400, 0x55_8C00, 0x1FF_FFFC] {
                assert_eq!(p.word(off) ^ p.opposite().word(off), u32::MAX, "{}", p.name());
            }
        }
    }

    #[test]
    fn rand_is_a_function_of_the_offset_only() {
        assert_eq!(Pattern::Rand(1).word(0x40), Pattern::Rand(1).word(0x40));
        assert_ne!(Pattern::Rand(1).word(0x40), Pattern::Rand(1).word(0x44));
        assert_ne!(Pattern::Rand(1).word(0x40), Pattern::Rand(2).word(0x40));
    }

    #[test]
    fn the_watch_class_is_the_v2_fingerprint() {
        // the five v2 flip addresses (#86) are 0x401 mod 0x800: the word below them is in the class
        for a in [0x55_8C01u32, 0x62_E401, 0x65_3C01, 0x7F_BC01] {
            assert_eq!(a & 0x7FF, 0x401);
        }
        assert!(in_watch_class(0x55_8C00));
        assert!(in_watch_class(0x62_E400));
        assert!(!in_watch_class(0x55_8C04));
        assert!(!in_watch_class(0x53_FC00)); // below the window
        assert!(!in_watch_class(0x64_0400)); // the window ends at 0x640000
        assert!(in_watch_class(0x63_FC00));
    }

    #[test]
    fn tally_counts_direction_class_and_first() {
        let mut t = Tally::new();
        t.check(0x55_8C00, 0, 0x100); // the v2 failure: bit 8, 0 -> 1, in class
        t.check(0x10, 0xFF, 0xFE); // bit 0, 1 -> 0, outside
        t.check(0x14, 5, 5);
        assert_eq!((t.words, t.bad, t.watch), (3, 2, 1));
        assert_eq!((t.up[8], t.down[0], t.up[0], t.down[8]), (1, 1, 0, 0));
        assert_eq!(t.first[0], (0x55_8C00, 0, 0x100));
        assert!(!t.clean());
    }

    #[test]
    fn merge_adds_and_keeps_the_first_six() {
        let mut a = Tally::new();
        let mut b = Tally::new();
        for i in 0..8u32 {
            a.check(i * 4, 0, 1);
            b.check(0x1000 + i * 4, 0, 2);
        }
        a.merge(&b);
        assert_eq!((a.words, a.bad), (16, 16));
        assert_eq!((a.up[0], a.up[1]), (8, 8));
        assert_eq!(a.first[5], (20, 0, 1)); // already full of a's: b adds none
        let mut c = Tally::new();
        c.merge(&b);
        assert_eq!(c.first[0], (0x1000, 0, 2));
        assert_eq!(c.first[5], (0x1014, 0, 2));
    }
}
