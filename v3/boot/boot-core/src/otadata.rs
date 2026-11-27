//! otadata and the boot order, as IDF `bootloader_utility_get_selected_boot_partition` (no anti-rollback, no
//! rollback-enable) decides it, then widened into a fallback list: this stage never writes otadata, so it tries the
//! chosen slot and then every other app slot until one verifies.
use crate::crc::crc32_le;
use crate::partition::{Entry, Table};

/// Each of the two otadata copies sits in its own 4 KB sector; the entry is the first 32 bytes.
pub const SECTOR: u32 = 0x1000;
pub const ENTRY_LEN: usize = 32;

pub const STATE_INVALID: u32 = 3;
pub const STATE_ABORTED: u32 = 4;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Select {
    pub seq: u32,
    pub state: u32,
    pub crc: u32,
}

impl Select {
    /// `ota_seq` u32, `seq_label` 20 bytes, `ota_state` u32, `crc` u32 (CRC of `ota_seq` only).
    pub fn parse(b: &[u8; ENTRY_LEN]) -> Select {
        let w = |i: usize| u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]]);
        Select { seq: w(0), state: w(24), crc: w(28) }
    }
    /// IDF `bootloader_common_ota_select_invalid`: erased sequence, or marked invalid or aborted.
    pub fn invalid(&self) -> bool {
        self.seq == u32::MAX || self.state == STATE_INVALID || self.state == STATE_ABORTED
    }
    pub fn crc_ok(&self) -> bool {
        self.crc == crc32_le(u32::MAX, &self.seq.to_le_bytes())
    }
    /// IDF `bootloader_common_ota_select_valid`.
    pub fn valid(&self) -> bool {
        !self.invalid() && self.crc_ok()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Slot {
    Factory,
    Ota(u8),
}

pub const MAX_SLOTS: usize = 17;

#[derive(Debug, Clone, Copy)]
pub struct BootOrder {
    slots: [Slot; MAX_SLOTS],
    count: usize,
    /// Which otadata copy (0 or 1) decided, `None` when the default order was used.
    pub active_otadata: Option<u8>,
}

impl BootOrder {
    pub fn slots(&self) -> &[Slot] {
        &self.slots[..self.count]
    }
    fn push(&mut self, s: Slot) {
        if self.count < MAX_SLOTS && !self.slots().contains(&s) {
            self.slots[self.count] = s;
            self.count += 1;
        }
    }
}

/// The partition a slot names, if the table has it.
pub fn resolve(table: &Table, slot: Slot) -> Option<&Entry> {
    match slot {
        Slot::Factory => table.factory(),
        Slot::Ota(n) => table.ota(n),
    }
}

/// `otadata` is `None` when the table has no otadata partition or it could not be read.
pub fn boot_order(table: &Table, otadata: Option<&[Select; 2]>) -> BootOrder {
    let count = table.ota_count();
    let default_first = if table.factory().is_some() { Slot::Factory } else { Slot::Ota(0) };
    let mut primary = default_first;
    let mut active = None;
    if let Some(sel) = otadata {
        if count > 0 && !(sel[0].invalid() && sel[1].invalid()) {
            let (v0, v1) = (sel[0].valid(), sel[1].valid());
            let pick = match (v0, v1) {
                (true, true) => Some(if sel[0].seq >= sel[1].seq { 0 } else { 1 }),
                (true, false) => Some(0),
                (false, true) => Some(1),
                (false, false) => None,
            };
            if let Some(i) = pick {
                active = Some(i as u8);
                primary = Slot::Ota((sel[i].seq.wrapping_sub(1) % count as u32) as u8);
            }
        }
    }
    let mut o = BootOrder { slots: [Slot::Factory; MAX_SLOTS], count: 0, active_otadata: active };
    o.push(primary);
    o.push(Slot::Factory);
    for n in 0..count {
        o.push(Slot::Ota(n));
    }
    // Only partitions that exist.
    let mut kept = BootOrder { slots: [Slot::Factory; MAX_SLOTS], count: 0, active_otadata: active };
    for &s in o.slots() {
        if resolve(table, s).is_some() {
            kept.push(s);
        }
    }
    kept
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::partition::{parse, tests::entry_bytes, tests::table_bytes};
    use proptest::prelude::*;
    use std::vec::Vec;

    fn sel(seq: u32, state: u32) -> Select {
        Select { seq, state, crc: crc32_le(u32::MAX, &seq.to_le_bytes()) }
    }
    const NEW: u32 = 0;
    const VALID: u32 = 2;
    const UNDEF: u32 = u32::MAX;

    fn two() -> Table {
        parse(&table_bytes(true)).unwrap()
    }

    #[test]
    fn parses_entry() {
        let mut b = [0xFFu8; 32];
        b[0..4].copy_from_slice(&3u32.to_le_bytes());
        b[24..28].copy_from_slice(&VALID.to_le_bytes());
        b[28..32].copy_from_slice(&crc32_le(u32::MAX, &3u32.to_le_bytes()).to_le_bytes());
        let s = Select::parse(&b);
        assert_eq!(s, sel(3, VALID));
        assert!(s.valid());
    }

    #[test]
    fn erased_otadata_defaults_to_ota_0_then_ota_1() {
        let e = Select { seq: u32::MAX, state: u32::MAX, crc: u32::MAX };
        let o = boot_order(&two(), Some(&[e, e]));
        assert_eq!(o.slots(), &[Slot::Ota(0), Slot::Ota(1)]);
        assert_eq!(o.active_otadata, None);
    }

    #[test]
    fn higher_sequence_wins_and_wraps_over_slots() {
        let t = two();
        let e = Select { seq: u32::MAX, state: UNDEF, crc: u32::MAX };
        assert_eq!(boot_order(&t, Some(&[sel(1, VALID), e])).slots(), &[Slot::Ota(0), Slot::Ota(1)]);
        assert_eq!(boot_order(&t, Some(&[sel(1, VALID), sel(2, NEW)])).slots(), &[Slot::Ota(1), Slot::Ota(0)]);
        assert_eq!(boot_order(&t, Some(&[sel(3, VALID), sel(2, VALID)])).slots(), &[Slot::Ota(0), Slot::Ota(1)]);
        assert_eq!(boot_order(&t, Some(&[sel(3, VALID), sel(2, VALID)])).active_otadata, Some(0));
        assert_eq!(boot_order(&t, Some(&[e, sel(4, VALID)])).slots(), &[Slot::Ota(1), Slot::Ota(0)]);
    }

    #[test]
    fn aborted_or_bad_crc_copy_is_ignored() {
        let t = two();
        assert_eq!(boot_order(&t, Some(&[sel(2, STATE_ABORTED), sel(1, VALID)])).slots()[0], Slot::Ota(0));
        assert_eq!(boot_order(&t, Some(&[sel(2, STATE_INVALID), sel(1, VALID)])).slots()[0], Slot::Ota(0));
        let mut bad = sel(2, VALID);
        bad.crc ^= 1;
        assert_eq!(boot_order(&t, Some(&[bad, sel(1, VALID)])).slots()[0], Slot::Ota(0));
        // both copies corrupt: default order, every slot still tried
        let mut bad1 = sel(1, VALID);
        bad1.crc ^= 1;
        let o = boot_order(&t, Some(&[bad, bad1]));
        assert_eq!(o.slots(), &[Slot::Ota(0), Slot::Ota(1)]);
        assert_eq!(o.active_otadata, None);
    }

    #[test]
    fn factory_comes_first_when_present_and_otadata_is_unusable() {
        let mut b = Vec::new();
        b.extend(entry_bytes(0, 0, 0x10000, 0x10000, "factory"));
        b.extend(entry_bytes(0, 0x10, 0x20000, 0x10000, "ota_0"));
        b.resize(3072, 0xFF);
        let t = parse(&b).unwrap();
        let e = Select { seq: u32::MAX, state: UNDEF, crc: u32::MAX };
        assert_eq!(boot_order(&t, Some(&[e, e])).slots(), &[Slot::Factory, Slot::Ota(0)]);
        assert_eq!(boot_order(&t, None).slots(), &[Slot::Factory, Slot::Ota(0)]);
        // a valid sequence still selects the OTA slot, factory stays as a fallback
        assert_eq!(boot_order(&t, Some(&[sel(1, VALID), e])).slots(), &[Slot::Ota(0), Slot::Factory]);
    }

    proptest! {
        /// Every slot in the order exists in the table, no slot repeats, and an app partition always yields a slot.
        #[test]
        fn order_is_sound(s0 in any::<(u32, u32, u32)>(), s1 in any::<(u32, u32, u32)>(), good in any::<bool>()) {
            let t = two();
            let mk = |(seq, state, crc): (u32, u32, u32)| if good { sel(seq, state) } else { Select { seq, state, crc } };
            let o = boot_order(&t, Some(&[mk(s0), mk(s1)]));
            prop_assert!(!o.slots().is_empty());
            for (i, s) in o.slots().iter().enumerate() {
                prop_assert!(resolve(&t, *s).is_some());
                prop_assert!(!o.slots()[..i].contains(s));
            }
        }
    }
}
