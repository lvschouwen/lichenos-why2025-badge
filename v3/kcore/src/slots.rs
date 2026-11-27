//! The ESP32-P4 app slot table (#612): the single source for where U text and data live in NOLOAD L2MEM. The linker
//! script's `.udata` (`_udata`) must equal `SLOT_DATA` (an ASSERT there and a CHECK row at boot).
use crate::image::Slot;

pub const SLOT_TEXT: usize = 0x4FF6_0000;
pub const SLOT_DATA: usize = 0x4FF7_0000;
pub const SLOT_LEN: usize = 0x8000;
/// Slots that exist. Core 1's U data page (0x4FF78000) is slot 1's data region.
pub const SLOTS: usize = 2;
/// NOLOAD memory ends here (platform mem.rs NOLOAD_LIMIT: the carve of the 256 KB reset-state cache).
pub const NOLOAD_LIMIT: usize = 0x4FF8_0000;

/// Slot `i`, or None past the table: callers can never be handed another slot's memory.
pub const fn slot(i: usize) -> Option<Slot> {
    if i >= SLOTS {
        return None;
    }
    Some(Slot { text: SLOT_TEXT + i * SLOT_LEN, data: SLOT_DATA + i * SLOT_LEN, len: SLOT_LEN })
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;

    #[test]
    fn no_slot_region_overlaps_another_and_all_end_inside_noload() {
        let mut regions = std::vec::Vec::new();
        for i in 0..SLOTS {
            let s = slot(i).unwrap();
            regions.push((s.text, s.text + s.len));
            regions.push((s.data, s.data + s.len));
        }
        for (i, a) in regions.iter().enumerate() {
            assert!(a.1 <= NOLOAD_LIMIT, "{a:x?} ends past NOLOAD_LIMIT");
            for b in &regions[i + 1..] {
                assert!(a.1 <= b.0 || b.1 <= a.0, "{a:x?} overlaps {b:x?}");
            }
        }
    }

    #[test]
    fn past_the_table_there_is_no_slot() {
        assert!(slot(SLOTS).is_none());
        assert!(slot(usize::MAX).is_none());
    }

    #[test]
    fn regions_are_napot_shaped() {
        for i in 0..SLOTS {
            let s = slot(i).unwrap();
            assert!(s.len.is_power_of_two() && s.text.is_multiple_of(s.len) && s.data.is_multiple_of(s.len));
        }
    }
}
