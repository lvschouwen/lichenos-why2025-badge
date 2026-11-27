//! BootInfo: what the stage hands the kernel in a1. Entry contract, both routes: a0 = hart id, a1 = a pointer to this
//! structure when the stage booted the kernel. Under load-ram the ROM leaves a junk a1 (0x14 was seen, #611), so the
//! kernel recognises a BootInfo by magic + address range + alignment, never by "a1 is not NULL".

pub const MAGIC: u32 = 0x3149_424C; // "LBI1" little-endian
pub const VERSION: u16 = 1;
pub const SIZE: usize = 64;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BootInfo {
    /// 0xFF = factory, else the OTA slot number.
    pub slot: u8,
    pub partition_offset: u32,
    pub partition_size: u32,
    pub image_len: u32,
    pub entry: u32,
    pub flags: u32,
    pub sha256: [u8; 32],
}

pub const SLOT_FACTORY: u8 = 0xFF;

impl BootInfo {
    /// Layout (little-endian): magic u32, version u16, size u16, slot u32, partition_offset, partition_size,
    /// image_len, entry, flags (u32 each), sha256 [32], then 4 reserved zero bytes. 64 bytes.
    pub fn to_bytes(&self) -> [u8; SIZE] {
        let mut b = [0u8; SIZE];
        b[0..4].copy_from_slice(&MAGIC.to_le_bytes());
        b[4..6].copy_from_slice(&VERSION.to_le_bytes());
        b[6..8].copy_from_slice(&(SIZE as u16).to_le_bytes());
        b[8..12].copy_from_slice(&(self.slot as u32).to_le_bytes());
        b[12..16].copy_from_slice(&self.partition_offset.to_le_bytes());
        b[16..20].copy_from_slice(&self.partition_size.to_le_bytes());
        b[20..24].copy_from_slice(&self.image_len.to_le_bytes());
        b[24..28].copy_from_slice(&self.entry.to_le_bytes());
        b[28..32].copy_from_slice(&self.flags.to_le_bytes());
        b[32..64].copy_from_slice(&self.sha256);
        b
    }

    /// `None` unless magic, version and size all match.
    pub fn from_bytes(b: &[u8; SIZE]) -> Option<BootInfo> {
        let w = |i: usize| u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]]);
        if w(0) != MAGIC
            || u16::from_le_bytes([b[4], b[5]]) != VERSION
            || u16::from_le_bytes([b[6], b[7]]) as usize != SIZE
        {
            return None;
        }
        if w(8) > 0xFF {
            return None;
        }
        let mut sha256 = [0u8; 32];
        sha256.copy_from_slice(&b[32..64]);
        Some(BootInfo {
            slot: w(8) as u8,
            partition_offset: w(12),
            partition_size: w(16),
            image_len: w(20),
            entry: w(24),
            flags: w(28),
            sha256,
        })
    }
}

/// Could `a1` point at a BootInfo? 4-byte aligned and the whole structure inside `[ram_start, ram_end)`.
/// Cheap, pure, and checked before the pointer is dereferenced.
pub fn plausible_pointer(a1: usize, ram_start: usize, ram_end: usize) -> bool {
    a1.is_multiple_of(4) && a1 >= ram_start && a1.checked_add(SIZE).is_some_and(|e| e <= ram_end)
}

/// Recognise a BootInfo at `a1`: `plausible_pointer`, then magic, version and size.
///
/// # Safety
/// If `a1` passes `plausible_pointer`, the `SIZE` bytes there must be readable (the range is real RAM). A junk a1
/// that fails the range check is never dereferenced.
pub unsafe fn recognise(a1: usize, ram_start: usize, ram_end: usize) -> Option<BootInfo> {
    if !plausible_pointer(a1, ram_start, ram_end) {
        return None;
    }
    // SAFETY: caller guarantees the range is readable RAM; `a1` is aligned, in range and SIZE bytes fit.
    let raw = unsafe { core::ptr::read_volatile(a1 as *const [u8; SIZE]) };
    BootInfo::from_bytes(&raw)
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    fn sample() -> BootInfo {
        BootInfo {
            slot: 1,
            partition_offset: 0x42_0000,
            partition_size: 0x40_0000,
            image_len: 0x1234,
            entry: 0x4FF1_0000,
            flags: 0,
            sha256: [0xAB; 32],
        }
    }

    #[test]
    fn roundtrip() {
        let b = sample().to_bytes();
        assert_eq!(&b[0..4], b"LBI1");
        assert_eq!(BootInfo::from_bytes(&b), Some(sample()));
    }

    #[test]
    fn rejects_wrong_tags() {
        let mut b = sample().to_bytes();
        b[0] ^= 1;
        assert_eq!(BootInfo::from_bytes(&b), None);
        let mut b = sample().to_bytes();
        b[4] = 9;
        assert_eq!(BootInfo::from_bytes(&b), None);
        let mut b = sample().to_bytes();
        b[6] = 63;
        assert_eq!(BootInfo::from_bytes(&b), None);
        let mut b = sample().to_bytes();
        b[9] = 1; // slot > 0xFF
        assert_eq!(BootInfo::from_bytes(&b), None);
    }

    #[test]
    fn junk_a1_is_not_a_pointer() {
        let (s, e) = (0x4FF0_0000, 0x4FFC_0000);
        assert!(!plausible_pointer(0, s, e));
        assert!(!plausible_pointer(0x14, s, e)); // what the ROM leaves under load-ram (#611)
        assert!(!plausible_pointer(0x4FF4_0001, s, e)); // misaligned
        assert!(!plausible_pointer(0x4FFB_FFE0, s, e)); // structure would cross the end
        assert!(!plausible_pointer(usize::MAX - 3, s, e));
        assert!(plausible_pointer(0x4FF4_0000, s, e));
        assert!(plausible_pointer(0x4FFB_FFC0, s, e));
    }

    #[test]
    fn recognise_reads_only_plausible_pointers() {
        // 0x14 and 0 are rejected before any dereference (this would segfault otherwise).
        // SAFETY: neither address passes the range check, so nothing is read.
        unsafe {
            assert_eq!(recognise(0x14, 0x1000, 0x2000), None);
            assert_eq!(recognise(0, 0x1000, 0x2000), None);
        }
        let mut buf = [0u32; 32];
        let bytes = sample().to_bytes();
        for (i, c) in bytes.chunks(4).enumerate() {
            buf[i] = u32::from_le_bytes([c[0], c[1], c[2], c[3]]);
        }
        let p = buf.as_ptr() as usize;
        // SAFETY: the range is exactly `buf`, which is live and aligned to 4.
        let got = unsafe { recognise(p, p, p + SIZE) };
        assert_eq!(got, Some(sample()));
    }

    proptest! {
        #[test]
        fn arbitrary_bytes_never_panic(b in proptest::array::uniform32(any::<u8>()), c in proptest::array::uniform32(any::<u8>())) {
            let mut raw = [0u8; SIZE];
            raw[..32].copy_from_slice(&b);
            raw[32..].copy_from_slice(&c);
            let _ = BootInfo::from_bytes(&raw);
        }

        #[test]
        fn plausible_never_overflows(a in any::<usize>(), s in any::<usize>(), e in any::<usize>()) {
            let _ = plausible_pointer(a, s, e);
        }
    }
}
