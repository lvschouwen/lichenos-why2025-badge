//! App image validation. An image is linked for one slot (fixed addresses, see apps/app.ld.in) and starts with a
//! header: magic, ABI version, entry, heap start, image end. Everything in the header is checked against the slot
//! before a single byte is copied; a bad image is refused and nothing runs.
use lichen_abi::{ABI_VERSION, IMAGE_MAGIC};

/// Room kept free for the app's stack at the top of its data region; sbrk may not grow into it.
pub const STACK_RESERVE: usize = 4096;
/// Header size in bytes: five little-endian words.
pub const HEADER_LEN: usize = 20;

/// The two fixed regions an app is linked for: text (R+X) and data (R+W), `len` bytes each.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Slot {
    pub text: usize,
    pub data: usize,
    pub len: usize,
}

#[derive(Debug, PartialEq, Eq)]
pub enum LoadError {
    TooSmall,
    TooBig,
    Magic,
    Abi,
    Entry,
    Heap,
    ImageEnd,
    /// The slot's last process ended with a forwarded call in flight: core 0 may still read its bounce buffer (#619).
    Busy,
}

fn word(b: &[u8], i: usize) -> usize {
    u32::from_le_bytes([b[4 * i], b[4 * i + 1], b[4 * i + 2], b[4 * i + 3]]) as usize
}

/// Check `blob` against slot `s`; returns (entry, heap start) when the image is acceptable.
pub fn validate(blob: &[u8], s: &Slot) -> Result<(usize, usize), LoadError> {
    if blob.len() < HEADER_LEN {
        return Err(LoadError::TooSmall);
    }
    if blob.len() > s.len {
        return Err(LoadError::TooBig);
    }
    if word(blob, 0) != IMAGE_MAGIC as usize {
        return Err(LoadError::Magic);
    }
    if word(blob, 1) != ABI_VERSION as usize {
        return Err(LoadError::Abi);
    }
    let (entry, heap, end) = (word(blob, 2), word(blob, 3), word(blob, 4));
    let text_end = s.text.saturating_add(blob.len());
    if entry < s.text.saturating_add(HEADER_LEN) || entry >= text_end || entry % 2 != 0 {
        return Err(LoadError::Entry);
    }
    let heap_max = s.data.saturating_add(s.len.saturating_sub(STACK_RESERVE));
    if heap < s.data || heap > heap_max {
        return Err(LoadError::Heap);
    }
    if end != text_end {
        return Err(LoadError::ImageEnd);
    }
    Ok((entry, heap))
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use proptest::prelude::*;
    use std::vec::Vec;

    const S: Slot = Slot { text: 0x8040_0000, data: 0x8080_0000, len: 0x1_0000 };

    fn image(words: [u32; 5], total: usize) -> Vec<u8> {
        let mut v: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        v.resize(total, 0);
        v
    }
    fn good() -> [u32; 5] {
        [IMAGE_MAGIC, ABI_VERSION, S.text as u32 + 20, S.data as u32, S.text as u32 + 64]
    }

    #[test]
    fn accepts_a_good_image() {
        assert_eq!(validate(&image(good(), 64), &S), Ok((S.text + 20, S.data)));
    }
    #[test]
    fn rejects_each_header_fault() {
        let mut w = good();
        w[0] = 0x4B4F_4F42;
        assert_eq!(validate(&image(w, 64), &S), Err(LoadError::Magic));
        let mut w = good();
        w[1] = 99;
        assert_eq!(validate(&image(w, 64), &S), Err(LoadError::Abi));
        let mut w = good();
        w[2] = S.text as u32 + 0x10_0000;
        assert_eq!(validate(&image(w, 64), &S), Err(LoadError::Entry));
        let mut w = good();
        w[2] = S.text as u32 + 20;
        assert_eq!(validate(&image(w, 20), &S), Err(LoadError::Entry), "entry at the image end (#656)");
        let mut w = good();
        w[2] = S.text as u32 + 21;
        assert_eq!(validate(&image(w, 64), &S), Err(LoadError::Entry), "odd entry");
        let mut w = good();
        w[3] = (S.data + S.len) as u32;
        assert_eq!(validate(&image(w, 64), &S), Err(LoadError::Heap));
        let mut w = good();
        w[4] = S.text as u32 + 60;
        assert_eq!(validate(&image(w, 64), &S), Err(LoadError::ImageEnd));
        assert_eq!(validate(&[0; 19], &S), Err(LoadError::TooSmall));
        assert_eq!(validate(&std::vec![0u8; S.len + 1], &S), Err(LoadError::TooBig));
    }
    /// A slot smaller than STACK_RESERVE used to underflow `len - STACK_RESERVE`.
    #[test]
    fn tiny_slot_does_not_underflow() {
        let tiny = Slot { text: 0x1000, data: 0x2000, len: 64 };
        let w = [IMAGE_MAGIC, ABI_VERSION, 0x1000 + 20, 0x2000, 0x1000 + 32];
        assert_eq!(validate(&image(w, 32), &tiny), Ok((0x1000 + 20, 0x2000)));
        let w = [IMAGE_MAGIC, ABI_VERSION, 0x1000 + 20, 0x2001, 0x1000 + 32];
        assert_eq!(validate(&image(w, 32), &tiny), Err(LoadError::Heap));
    }

    proptest! {
        /// Whatever the bytes and the slot, validate never panics, and an accepted image keeps every promise the
        /// loader relies on before it copies anything.
        #[test]
        fn validate_never_panics_and_accepted_images_are_inside_the_slot(
            blob in proptest::collection::vec(any::<u8>(), 0..96),
            text in 0usize..0x8000_0000, data in 0usize..0x8000_0000, len in 0usize..0x2_0000,
            hdr in proptest::option::of((any::<u32>(), any::<u32>(), any::<u32>())),
        ) {
            let s = Slot { text, data, len };
            let mut blob = blob;
            if let (Some((e, h, end)), true) = (hdr, blob.len() >= HEADER_LEN) {
                blob[0..4].copy_from_slice(&IMAGE_MAGIC.to_le_bytes());
                blob[4..8].copy_from_slice(&ABI_VERSION.to_le_bytes());
                blob[8..12].copy_from_slice(&e.to_le_bytes());
                blob[12..16].copy_from_slice(&h.to_le_bytes());
                blob[16..20].copy_from_slice(&end.to_le_bytes());
            }
            if let Ok((entry, heap)) = validate(&blob, &s) {
                prop_assert!(blob.len() <= s.len);
                prop_assert!(entry >= s.text + HEADER_LEN && entry < s.text + blob.len() && entry % 2 == 0);
                prop_assert!(heap >= s.data && heap + STACK_RESERVE <= s.data + s.len);
            }
        }
        #[test]
        // The entry sits at text + 20 (just past the header), so the image must hold at least one 2-byte instruction
        // after it: total 20 puts the entry at the image end, which `validate` rightly refuses (#656).
        fn a_header_built_from_the_slot_is_accepted(total in 22usize..0x1_0000, heap_off in 0usize..(0x1_0000 - 4096)) {
            let w = [IMAGE_MAGIC, ABI_VERSION, S.text as u32 + 20, (S.data + heap_off) as u32, (S.text + total) as u32];
            prop_assert_eq!(validate(&image(w, total), &S), Ok((S.text + 20, S.data + heap_off)));
        }
    }
}
