//! Partition table (IDF `esp_flash_partitions.h`): 32-byte entries at 0x8000, magic 0x50AA (bytes AA 50), an optional
//! MD5 entry (bytes EB EB, digest in bytes 16..32 over all entries before it), then 0xFF.
use md5::{Digest, Md5};

pub const TABLE_OFFSET: u32 = 0x8000;
pub const ENTRY_LEN: usize = 32;
pub const MAX_ENTRIES: usize = 95;
/// Bytes to read for the whole table (the sector's first 3 KB, as the IDF bootloader does).
pub const TABLE_LEN: usize = MAX_ENTRIES * ENTRY_LEN + ENTRY_LEN;

pub const TYPE_APP: u8 = 0x00;
pub const TYPE_DATA: u8 = 0x01;
pub const SUBTYPE_FACTORY: u8 = 0x00;
pub const SUBTYPE_OTA_0: u8 = 0x10;
pub const SUBTYPE_OTA_MAX: u8 = 0x1F;
pub const SUBTYPE_DATA_OTA: u8 = 0x00;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Entry {
    pub ptype: u8,
    pub subtype: u8,
    pub offset: u32,
    pub size: u32,
    pub name: [u8; 16],
    pub flags: u32,
}

const EMPTY: Entry = Entry { ptype: 0, subtype: 0, offset: 0, size: 0, name: [0; 16], flags: 0 };

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PartError {
    /// Entry `index` has neither the partition, the MD5 nor the end magic.
    BadMagic(usize),
    /// More than `MAX_ENTRIES` entries before the end.
    TooMany,
    /// The MD5 entry does not match the entries before it.
    BadMd5,
    /// Entry `index` has size 0 or `offset + size` overflows.
    BadRange(usize),
    /// No entries at all (erased flash reads as one).
    Empty,
}

#[derive(Debug, Clone, Copy)]
pub struct Table {
    entries: [Entry; MAX_ENTRIES],
    count: usize,
    pub md5_checked: bool,
}

impl Table {
    pub fn entries(&self) -> &[Entry] {
        &self.entries[..self.count]
    }
    pub fn find(&self, ptype: u8, subtype: u8) -> Option<&Entry> {
        self.entries().iter().find(|e| e.ptype == ptype && e.subtype == subtype)
    }
    pub fn otadata(&self) -> Option<&Entry> {
        self.find(TYPE_DATA, SUBTYPE_DATA_OTA)
    }
    pub fn factory(&self) -> Option<&Entry> {
        self.find(TYPE_APP, SUBTYPE_FACTORY)
    }
    /// The OTA app partition with subtype `ota_0 + n`.
    pub fn ota(&self, n: u8) -> Option<&Entry> {
        if n > SUBTYPE_OTA_MAX - SUBTYPE_OTA_0 {
            return None;
        }
        self.find(TYPE_APP, SUBTYPE_OTA_0 + n)
    }
    /// Number of consecutive OTA app partitions from ota_0 (IDF `app_count`; slots are numbered by subtype).
    pub fn ota_count(&self) -> u8 {
        (0..=SUBTYPE_OTA_MAX - SUBTYPE_OTA_0).take_while(|&n| self.ota(n).is_some()).count() as u8
    }
}

/// Parse the table bytes read from `TABLE_OFFSET`. Stops at the first 0xFFFF entry; a missing MD5 entry is accepted
/// (old tables) and reported through `md5_checked`.
pub fn parse(bytes: &[u8]) -> Result<Table, PartError> {
    let mut t = Table { entries: [EMPTY; MAX_ENTRIES], count: 0, md5_checked: false };
    let mut md5 = Md5::new();
    let mut i = 0;
    while let Some(raw) = bytes.get(i * ENTRY_LEN..(i + 1) * ENTRY_LEN) {
        match (raw[0], raw[1]) {
            (0xAA, 0x50) => {
                if t.count == MAX_ENTRIES {
                    return Err(PartError::TooMany);
                }
                let e = Entry {
                    ptype: raw[2],
                    subtype: raw[3],
                    offset: u32::from_le_bytes([raw[4], raw[5], raw[6], raw[7]]),
                    size: u32::from_le_bytes([raw[8], raw[9], raw[10], raw[11]]),
                    name: raw[12..28].try_into().unwrap_or([0; 16]),
                    flags: u32::from_le_bytes([raw[28], raw[29], raw[30], raw[31]]),
                };
                if e.size == 0 || e.offset.checked_add(e.size).is_none() {
                    return Err(PartError::BadRange(i));
                }
                md5.update(raw);
                t.entries[t.count] = e;
                t.count += 1;
            }
            (0xEB, 0xEB) => {
                let want: [u8; 16] = raw[16..32].try_into().unwrap_or([0; 16]);
                let got: [u8; 16] = md5.finalize().into();
                if want != got {
                    return Err(PartError::BadMd5);
                }
                t.md5_checked = true;
                break;
            }
            (0xFF, 0xFF) => break,
            _ => return Err(PartError::BadMagic(i)),
        }
        i += 1;
    }
    if t.count == 0 {
        return Err(PartError::Empty);
    }
    Ok(t)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use proptest::prelude::*;
    use std::vec;
    use std::vec::Vec;

    pub fn entry_bytes(ptype: u8, subtype: u8, offset: u32, size: u32, name: &str) -> [u8; 32] {
        let mut e = [0u8; 32];
        e[0] = 0xAA;
        e[1] = 0x50;
        e[2] = ptype;
        e[3] = subtype;
        e[4..8].copy_from_slice(&offset.to_le_bytes());
        e[8..12].copy_from_slice(&size.to_le_bytes());
        e[12..12 + name.len()].copy_from_slice(name.as_bytes());
        e
    }

    /// A table shaped like the badge's: nvs, otadata, ota_0, ota_1, with an MD5 entry when `md5`.
    pub fn table_bytes(md5: bool) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend(entry_bytes(1, 2, 0xB000, 0x6000, "nvs"));
        b.extend(entry_bytes(1, 0, 0x11000, 0x2000, "otadata"));
        b.extend(entry_bytes(0, 0x10, 0x20000, 0x40_0000, "ota_0"));
        b.extend(entry_bytes(0, 0x11, 0x42_0000, 0x40_0000, "ota_1"));
        if md5 {
            let d: [u8; 16] = Md5::digest(&b).into();
            let mut m = [0xFFu8; 32];
            m[0] = 0xEB;
            m[1] = 0xEB;
            m[16..].copy_from_slice(&d);
            b.extend(m);
        }
        b.resize(TABLE_LEN, 0xFF);
        b
    }

    #[test]
    fn parses_badge_like_table() {
        let t = parse(&table_bytes(true)).unwrap();
        assert!(t.md5_checked);
        assert_eq!(t.entries().len(), 4);
        assert_eq!(t.otadata().unwrap().offset, 0x11000);
        assert_eq!(t.ota(0).unwrap().offset, 0x20000);
        assert_eq!(t.ota(1).unwrap().subtype, 0x11);
        assert_eq!(t.ota_count(), 2);
        assert!(t.factory().is_none());
        assert_eq!(&t.entries()[2].name[..5], b"ota_0");
    }

    #[test]
    fn table_without_md5_is_accepted_but_flagged() {
        let t = parse(&table_bytes(false)).unwrap();
        assert!(!t.md5_checked);
        assert_eq!(t.entries().len(), 4);
    }

    #[test]
    fn rejects_bad_tables() {
        let mut b = table_bytes(true);
        b[8] ^= 0x10; // an entry changed under the MD5
        assert_eq!(parse(&b).unwrap_err(), PartError::BadMd5);
        let mut b = table_bytes(true);
        b[2 * 32] = 0x12;
        assert_eq!(parse(&b).unwrap_err(), PartError::BadMagic(2));
        assert_eq!(parse(&vec![0xFF; TABLE_LEN]).unwrap_err(), PartError::Empty);
        let mut b = table_bytes(false);
        b[8..12].copy_from_slice(&0u32.to_le_bytes());
        assert_eq!(parse(&b).unwrap_err(), PartError::BadRange(0));
        let mut b = table_bytes(false);
        b[4..8].copy_from_slice(&0xFFFF_F000u32.to_le_bytes());
        assert_eq!(parse(&b).unwrap_err(), PartError::BadRange(0));
    }

    #[test]
    fn too_many_entries() {
        let mut b = Vec::new();
        for _ in 0..=MAX_ENTRIES {
            b.extend(entry_bytes(1, 2, 0xB000, 0x1000, "x"));
        }
        assert_eq!(parse(&b).unwrap_err(), PartError::TooMany);
    }

    proptest! {
        #[test]
        fn arbitrary_bytes_never_panic(bytes in proptest::collection::vec(any::<u8>(), 0..3300)) {
            let _ = parse(&bytes);
        }
    }
}
