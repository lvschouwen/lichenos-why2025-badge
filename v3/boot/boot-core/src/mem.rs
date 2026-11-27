//! Memory map and segment bounds. A segment may only be copied into an allowlisted RAM range, and never over the
//! stage's own ROM-load window.

/// Half-open address range `[start, end)`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Range {
    pub start: u32,
    pub end: u32,
}

impl Range {
    pub const fn new(start: u32, end: u32) -> Self {
        Range { start, end }
    }
    /// `[addr, addr+len)` lies inside this range. `None`-safe: an overflowing segment is never inside.
    pub fn contains(&self, addr: u32, len: u32) -> bool {
        match addr.checked_add(len) {
            Some(e) => addr >= self.start && e <= self.end,
            None => false,
        }
    }
    /// `[addr, addr+len)` shares a byte with this range. An empty segment overlaps nothing.
    pub fn overlaps(&self, addr: u32, len: u32) -> bool {
        match addr.checked_add(len) {
            Some(e) => len != 0 && addr < self.end && e > self.start,
            None => true,
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct MemoryMap<'a> {
    /// Where a loaded segment may go.
    pub ram: &'a [Range],
    /// Never loaded over (the stage itself); checked before `ram`.
    pub forbidden: &'a [Range],
    /// Flash-mapped windows: a segment there belongs to a flash-mapped (IDF style) image, which this stage cannot copy.
    pub mapped: &'a [Range],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SegClass {
    /// Empty, or fully inside an allowlisted RAM range.
    Ram,
    /// Inside a flash-mapped window: valid for an IDF image, not loadable here.
    Mapped,
    /// Overlaps a forbidden range.
    Forbidden,
    /// Address arithmetic overflows.
    Overflow,
    /// Outside everything the map names.
    Outside,
}

impl MemoryMap<'_> {
    pub fn classify(&self, addr: u32, len: u32) -> SegClass {
        if addr.checked_add(len).is_none() {
            return SegClass::Overflow;
        }
        if self.forbidden.iter().any(|r| r.overlaps(addr, len)) {
            return SegClass::Forbidden;
        }
        if len == 0 || self.ram.iter().any(|r| r.contains(addr, len)) {
            return SegClass::Ram;
        }
        if self.mapped.iter().any(|r| r.contains(addr, len)) {
            return SegClass::Mapped;
        }
        SegClass::Outside
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const RAM: [Range; 1] = [Range::new(0x4FF1_0000, 0x4FF2_8ED0)];
    const FORB: [Range; 1] = [Range::new(0x4FF2_8ED0, 0x4FF3_ABD0)];
    const MAPPED: [Range; 1] = [Range::new(0x4000_0000, 0x4C00_0000)];
    const MAP: MemoryMap = MemoryMap { ram: &RAM, forbidden: &FORB, mapped: &MAPPED };

    #[test]
    fn classes() {
        assert_eq!(MAP.classify(0x4FF1_0000, 0x100), SegClass::Ram);
        assert_eq!(MAP.classify(0x4FF2_8E00, 0xD0), SegClass::Ram); // ends exactly at the window
        assert_eq!(MAP.classify(0x4FF2_8E00, 0xD1), SegClass::Forbidden);
        assert_eq!(MAP.classify(0x4FF2_8ED0, 1), SegClass::Forbidden);
        assert_eq!(MAP.classify(0x4FF3_ABCF, 1), SegClass::Forbidden);
        assert_eq!(MAP.classify(0x4FF3_ABD0, 4), SegClass::Outside);
        assert_eq!(MAP.classify(0x4FF0_FFFC, 8), SegClass::Outside); // starts below the RAM range
        assert_eq!(MAP.classify(0x4200_0020, 0x1000), SegClass::Mapped);
        assert_eq!(MAP.classify(0xFFFF_FFF0, 0x20), SegClass::Overflow);
        assert_eq!(MAP.classify(0x1234, 0), SegClass::Ram);
    }
}
