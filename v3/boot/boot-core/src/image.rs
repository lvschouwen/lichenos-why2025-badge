//! ESP image format (`esptool elf2image`, IDF `esp_image_format.c`): parse and verify, streaming from flash.
//!
//! Layout: 24-byte header, `segment_count` x (load_addr u32, data_len u32, data), zero padding so the checksum byte is
//! the last byte of a 16-byte block, the checksum byte (0xEF xor all segment data), and when `hash_appended` is 1 a
//! SHA-256 of every byte before it (header through checksum). Integrity (`verify`) and loadability (`check_loadable`)
//! are separate results: an IDF v2 image verifies fine and is still not loadable by this stage.
use crate::flash::{FlashError, FlashRead};
use crate::mem::{MemoryMap, SegClass};
use sha2::{Digest, Sha256};

pub const MAGIC: u8 = 0xE9;
pub const HEADER_LEN: u32 = 24;
pub const MAX_SEGMENTS: usize = 16;
pub const CHIP_ID_ESP32P4: u16 = 18;
const CHECKSUM_INIT: u8 = 0xEF;
const CHUNK: usize = 256;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ImageError {
    Flash,
    /// First byte is not 0xE9. 0xFF means erased flash.
    BadMagic(u8),
    BadSegmentCount(u8),
    WrongChip(u16),
    /// Segment length not a multiple of 4.
    SegmentUnaligned,
    /// The image (or one segment) runs past the partition.
    TooLong,
    BadChecksum,
    /// `hash_appended` is not 0 or 1.
    BadHashFlag(u8),
    BadHash,
    /// `require_hash` was set and the image has no appended SHA-256.
    NoHash,
}

impl From<FlashError> for ImageError {
    fn from(_: FlashError) -> Self {
        ImageError::Flash
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Segment {
    pub load_addr: u32,
    pub len: u32,
    /// Offset of the segment data from the image start.
    pub data_off: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Verified {
    pub entry: u32,
    pub chip_id: u16,
    pub min_rev_full: u16,
    pub max_rev_full: u16,
    pub segment_count: usize,
    pub segments: [Segment; MAX_SEGMENTS],
    /// Bytes from the image start through the checksum byte and the appended hash, if any.
    pub image_len: u32,
    pub hash_appended: bool,
    /// SHA-256 of header..checksum: the appended digest when present and matching (it is checked), else computed.
    pub sha256: [u8; 32],
}

impl Verified {
    pub fn segments(&self) -> &[Segment] {
        &self.segments[..self.segment_count]
    }
}

/// What `verify` expects of an image.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VerifyOpts {
    /// The chip id the header must carry (`CHIP_ID_ESP32P4` for the P4 stage; the C6 stage passes its own).
    pub chip_id: u16,
    /// Refuse an image without an appended SHA-256 (`NoHash`). The jump path sets this; the proof may report
    /// "checksum only" images and so clears it.
    pub require_hash: bool,
}

/// Parse and verify the image at flash `base`, which may be at most `max_len` bytes (the partition size).
/// `tick` runs once per chunk read, for the watchdog.
pub fn verify<F: FlashRead>(
    flash: &mut F,
    base: u32,
    max_len: u32,
    opts: VerifyOpts,
    tick: &mut dyn FnMut(),
) -> Result<Verified, ImageError> {
    let mut sha = Sha256::new();
    let mut hdr = [0u8; HEADER_LEN as usize];
    if max_len < HEADER_LEN {
        return Err(ImageError::TooLong);
    }
    flash.read(base, &mut hdr)?;
    if hdr[0] != MAGIC {
        return Err(ImageError::BadMagic(hdr[0]));
    }
    let n = hdr[1];
    if n == 0 || n as usize > MAX_SEGMENTS {
        return Err(ImageError::BadSegmentCount(n));
    }
    let chip_id = u16::from_le_bytes([hdr[12], hdr[13]]);
    if chip_id != opts.chip_id {
        return Err(ImageError::WrongChip(chip_id));
    }
    if hdr[23] > 1 {
        return Err(ImageError::BadHashFlag(hdr[23]));
    }
    if opts.require_hash && hdr[23] != 1 {
        return Err(ImageError::NoHash);
    }
    sha.update(hdr);
    let mut v = Verified {
        entry: u32::from_le_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]),
        chip_id,
        min_rev_full: u16::from_le_bytes([hdr[15], hdr[16]]),
        max_rev_full: u16::from_le_bytes([hdr[17], hdr[18]]),
        segment_count: n as usize,
        segments: [Segment::default(); MAX_SEGMENTS],
        image_len: 0,
        hash_appended: hdr[23] == 1,
        sha256: [0; 32],
    };
    let mut pos = HEADER_LEN; // offset from base, always <= max_len
    let mut xor = CHECKSUM_INIT;
    for i in 0..n as usize {
        if max_len - pos < 8 {
            return Err(ImageError::TooLong);
        }
        let mut sh = [0u8; 8];
        flash.read(base + pos, &mut sh)?;
        sha.update(sh);
        let load_addr = u32::from_le_bytes([sh[0], sh[1], sh[2], sh[3]]);
        let len = u32::from_le_bytes([sh[4], sh[5], sh[6], sh[7]]);
        pos += 8;
        if !len.is_multiple_of(4) {
            return Err(ImageError::SegmentUnaligned);
        }
        if len > max_len - pos {
            return Err(ImageError::TooLong);
        }
        v.segments[i] = Segment { load_addr, len, data_off: pos };
        let mut left = len;
        let mut off = base + pos;
        let mut buf = [0u8; CHUNK];
        while left > 0 {
            let k = left.min(CHUNK as u32) as usize;
            flash.read(off, &mut buf[..k])?;
            sha.update(&buf[..k]);
            for &b in &buf[..k] {
                xor ^= b;
            }
            off += k as u32;
            left -= k as u32;
            tick();
        }
        pos += len;
    }
    // Padding up to the checksum byte, the last byte of the 16-byte block.
    let pad = 15 - (pos % 16);
    if max_len - pos < pad + 1 {
        return Err(ImageError::TooLong);
    }
    let mut tail = [0u8; 16];
    let tail = &mut tail[..pad as usize + 1];
    flash.read(base + pos, tail)?;
    sha.update(&*tail);
    if tail[pad as usize] != xor {
        return Err(ImageError::BadChecksum);
    }
    pos += pad + 1;
    let digest: [u8; 32] = sha.finalize().into();
    if v.hash_appended {
        if max_len - pos < 32 {
            return Err(ImageError::TooLong);
        }
        let mut want = [0u8; 32];
        flash.read(base + pos, &mut want)?;
        if want != digest {
            return Err(ImageError::BadHash);
        }
        pos += 32;
    }
    v.image_len = pos;
    v.sha256 = digest;
    Ok(v)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoadError {
    /// Segment `index` is not `Ram` per the memory map.
    Segment { index: usize, class: SegClass },
    /// The entry point is not inside a loaded (non-empty) segment.
    EntryOutsideSegments,
}

/// Can this stage copy the image into RAM and jump? Every segment must be `Ram`, none may touch a forbidden range,
/// and the entry must lie inside one of them.
pub fn check_loadable(v: &Verified, map: &MemoryMap) -> Result<(), LoadError> {
    for (index, s) in v.segments().iter().enumerate() {
        match map.classify(s.load_addr, s.len) {
            SegClass::Ram => {}
            class => return Err(LoadError::Segment { index, class }),
        }
    }
    let entry_ok = v.segments().iter().any(|s| s.len != 0 && s.load_addr <= v.entry && v.entry - s.load_addr < s.len);
    if entry_ok {
        Ok(())
    } else {
        Err(LoadError::EntryOutsideSegments)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mem::Range;
    use crate::testutil::{build_image, VecFlash};
    use proptest::prelude::*;
    use std::vec;
    use std::vec::Vec;

    fn nop() {}

    const OPTS: VerifyOpts = VerifyOpts { chip_id: CHIP_ID_ESP32P4, require_hash: false };

    fn segs() -> Vec<(u32, Vec<u8>)> {
        vec![(0x4FF1_0000, (0..100u8).collect()), (0x4FF1_1000, vec![7u8; 36])]
    }

    #[test]
    fn good_image_with_hash() {
        let img = build_image(0x4FF1_0000, &segs(), true);
        let len = img.len() as u32;
        let mut f = VecFlash::at(0x20000, img.clone());
        let v = verify(&mut f, 0x20000, 0x10_0000, OPTS, &mut nop).unwrap();
        assert_eq!(v.segment_count, 2);
        assert_eq!(v.segments()[1], Segment { load_addr: 0x4FF1_1000, len: 36, data_off: 24 + 8 + 100 + 8 });
        assert_eq!(v.image_len, len);
        assert!(v.hash_appended);
        assert_eq!(&v.sha256[..], &img[img.len() - 32..]);
    }

    #[test]
    fn good_image_without_hash() {
        let img = build_image(0x4FF1_0000, &segs(), false);
        let mut f = VecFlash::at(0, img.clone());
        let v = verify(&mut f, 0, img.len() as u32, OPTS, &mut nop).unwrap();
        assert!(!v.hash_appended);
        assert_eq!(v.image_len as usize, img.len());
    }

    #[test]
    fn rejects_corruption() {
        let img = build_image(0x4FF1_0000, &segs(), true);
        let n = img.len() as u32;
        // one flipped data byte: the xor checksum catches it first
        let mut bad = img.clone();
        bad[40] ^= 1;
        assert_eq!(verify(&mut VecFlash::at(0, bad), 0, n, OPTS, &mut nop), Err(ImageError::BadChecksum));
        // a flipped header byte keeps the checksum but breaks the SHA
        let mut bad = img.clone();
        bad[4] ^= 1;
        assert_eq!(verify(&mut VecFlash::at(0, bad), 0, n, OPTS, &mut nop), Err(ImageError::BadHash));
        // flipped digest byte
        let mut bad = img.clone();
        *bad.last_mut().unwrap() ^= 1;
        assert_eq!(verify(&mut VecFlash::at(0, bad), 0, n, OPTS, &mut nop), Err(ImageError::BadHash));
        // magic
        let mut bad = img.clone();
        bad[0] = 0xFF;
        assert_eq!(verify(&mut VecFlash::at(0, bad), 0, n, OPTS, &mut nop), Err(ImageError::BadMagic(0xFF)));
        // wrong chip
        let mut bad = img.clone();
        bad[12] = 5;
        assert_eq!(verify(&mut VecFlash::at(0, bad), 0, n, OPTS, &mut nop), Err(ImageError::WrongChip(5)));
        // too long for its partition
        assert_eq!(verify(&mut VecFlash::at(0, img.clone()), 0, n - 1, OPTS, &mut nop), Err(ImageError::TooLong));
        // flash error
        assert_eq!(verify(&mut VecFlash::at(0, img[..60].to_vec()), 0, n, OPTS, &mut nop), Err(ImageError::Flash));
    }

    #[test]
    fn require_hash_refuses_checksum_only_images() {
        let strict = VerifyOpts { require_hash: true, ..OPTS };
        let plain = build_image(0x4FF1_0000, &segs(), false);
        let n = plain.len() as u32;
        assert_eq!(
            verify(&mut VecFlash::at(0, plain.clone()), 0, n, OPTS, &mut nop).map(|v| v.hash_appended),
            Ok(false)
        );
        assert_eq!(verify(&mut VecFlash::at(0, plain), 0, n, strict, &mut nop), Err(ImageError::NoHash));
        let hashed = build_image(0x4FF1_0000, &segs(), true);
        let n = hashed.len() as u32;
        assert!(verify(&mut VecFlash::at(0, hashed), 0, n, strict, &mut nop).is_ok());
    }

    #[test]
    fn chip_id_is_a_parameter() {
        let img = build_image(0x4FF1_0000, &segs(), true); // chip id 18
        let n = img.len() as u32;
        let c6 = VerifyOpts { chip_id: 13, require_hash: true };
        assert_eq!(verify(&mut VecFlash::at(0, img), 0, n, c6, &mut nop), Err(ImageError::WrongChip(18)));
    }

    #[test]
    fn refuses_unaligned_load_address() {
        let s = vec![(0x4FF1_0002, vec![0u8; 8])];
        assert_eq!(
            check_loadable(&verified(0x4FF1_0002, &s), &MAP),
            Err(LoadError::Segment { index: 0, class: SegClass::Unaligned })
        );
    }

    #[test]
    fn rejects_shape() {
        let mut img = build_image(0x4FF1_0000, &segs(), true);
        img[1] = 0;
        let n = img.len() as u32;
        assert_eq!(
            verify(&mut VecFlash::at(0, img.clone()), 0, n, OPTS, &mut nop),
            Err(ImageError::BadSegmentCount(0))
        );
        img[1] = 17;
        assert_eq!(
            verify(&mut VecFlash::at(0, img.clone()), 0, n, OPTS, &mut nop),
            Err(ImageError::BadSegmentCount(17))
        );
        img[1] = 2;
        img[23] = 2;
        assert_eq!(verify(&mut VecFlash::at(0, img.clone()), 0, n, OPTS, &mut nop), Err(ImageError::BadHashFlag(2)));
        img[23] = 1;
        img[24 + 4] = 99; // data_len 99, not a multiple of 4
        assert_eq!(verify(&mut VecFlash::at(0, img.clone()), 0, n, OPTS, &mut nop), Err(ImageError::SegmentUnaligned));
        img[24 + 4] = 0xFC;
        img[24 + 7] = 0xFF; // huge segment length
        assert_eq!(verify(&mut VecFlash::at(0, img), 0, n, OPTS, &mut nop), Err(ImageError::TooLong));
    }

    const RAM: [Range; 1] = [Range::new(0x4FF1_0000, 0x4FF2_8ED0)];
    const FORB: [Range; 1] = [Range::new(0x4FF2_8ED0, 0x4FF3_ABD0)];
    const MAPPED: [Range; 1] = [Range::new(0x4000_0000, 0x4C00_0000)];
    const MAP: MemoryMap = MemoryMap { ram: &RAM, forbidden: &FORB, mapped: &MAPPED };

    fn verified(entry: u32, segs: &[(u32, Vec<u8>)]) -> Verified {
        let img = build_image(entry, segs, true);
        verify(&mut VecFlash::at(0, img.clone()), 0, img.len() as u32, OPTS, &mut nop).unwrap()
    }

    #[test]
    fn loadable() {
        assert_eq!(check_loadable(&verified(0x4FF1_0000, &segs()), &MAP), Ok(()));
    }

    #[test]
    fn refuses_the_stage_window() {
        let s = vec![(0x4FF1_0000, vec![0u8; 8]), (0x4FF2_8ECC, vec![0u8; 8])];
        assert_eq!(
            check_loadable(&verified(0x4FF1_0000, &s), &MAP),
            Err(LoadError::Segment { index: 1, class: SegClass::Forbidden })
        );
    }

    #[test]
    fn refuses_mapped_and_stray_entry() {
        let s = vec![(0x4200_0020, vec![0u8; 8])];
        assert_eq!(
            check_loadable(&verified(0x4200_0020, &s), &MAP),
            Err(LoadError::Segment { index: 0, class: SegClass::Mapped })
        );
        assert_eq!(check_loadable(&verified(0x4FF1_0100, &segs()), &MAP), Err(LoadError::EntryOutsideSegments));
    }

    proptest! {
        /// Arbitrary bytes in flash never panic, and accepted images obey their own shape.
        #[test]
        fn arbitrary_bytes_never_panic(bytes in proptest::collection::vec(any::<u8>(), 0..600), max in 0u32..2000) {
            let mut f = VecFlash::at(0, bytes);
            if let Ok(v) = verify(&mut f, 0, max, OPTS, &mut nop) {
                prop_assert!(v.image_len <= max);
            }
        }

        /// Headers that pass the magic/chip gate and carry random segment tables still never panic.
        #[test]
        fn plausible_header_never_panics(tail in proptest::collection::vec(any::<u8>(), 0..400), nseg in 0u8..20, hash in 0u8..3) {
            let mut b = vec![0u8; 24];
            b[0] = 0xE9; b[1] = nseg; b[12] = 18; b[23] = hash;
            b.extend(tail);
            let n = b.len() as u32;
            let mut f = VecFlash::at(0, b);
            let _ = verify(&mut f, 0, n, OPTS, &mut nop);
        }

        /// Accepted by check_loadable implies every non-empty segment is inside the RAM allowlist and clear of the window.
        #[test]
        fn loadable_implies_inside_map(addrs in proptest::collection::vec((any::<u32>(), 0usize..16), 1..6), entry in any::<u32>()) {
            let s: Vec<(u32, Vec<u8>)> = addrs.iter().map(|&(a, w)| (a, vec![1u8; w * 4])).collect();
            let img = build_image(entry, &s, true);
            let n = img.len() as u32;
            let v = verify(&mut VecFlash::at(0, img), 0, n, OPTS, &mut nop).unwrap();
            if check_loadable(&v, &MAP).is_ok() {
                for sg in v.segments() {
                    if sg.len != 0 {
                        prop_assert!(sg.load_addr >= 0x4FF1_0000 && sg.load_addr as u64 + sg.len as u64 <= 0x4FF2_8ED0);
                    }
                }
            }
        }
    }
}
