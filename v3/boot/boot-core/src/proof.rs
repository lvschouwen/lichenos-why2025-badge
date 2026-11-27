//! The step-1 proof (#615): read the partition table and otadata, work out the boot order, SHA-verify every app slot
//! and report. Read-only and it never jumps. Every step prints its marker before it starts, so a dead run shows where.
use crate::console::Console;
use crate::flash::FlashRead;
use crate::image::{self, ImageError, LoadError, Verified};
use crate::mem::{MemoryMap, SegClass};
use crate::otadata::{self, Select, Slot};
use crate::partition::{self, Entry, PartError, Table};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Outcome {
    /// The first slot of the boot order that verified, if any.
    pub chosen: Option<Slot>,
    pub verified: u8,
    pub failed: u8,
}

pub fn run<F: FlashRead, C: Console>(flash: &mut F, con: &mut C, map: &MemoryMap, tick: &mut dyn FnMut()) -> Outcome {
    let mut out = Outcome { chosen: None, verified: 0, failed: 0 };
    con.line("LBOOT proof v1: read-only, no jump");
    con.line("step: partition table");
    let table = match read_table(flash) {
        Ok(t) => t,
        Err(e) => {
            con.puts("FAIL partition table: ");
            con.line(part_err(e));
            return end(con, out);
        }
    };
    print_table(con, &table);
    con.line("step: otadata");
    let sel = read_otadata(flash, &table, con);
    let order = otadata::boot_order(&table, sel.as_ref());
    con.puts("order:");
    for &s in order.slots() {
        con.put(b' ');
        slot_name(con, s);
    }
    con.puts(" (otadata copy ");
    match order.active_otadata {
        Some(i) => con.dec(i as u32),
        None => con.puts("none"),
    }
    con.line(")");
    for &slot in order.slots() {
        let Some(p) = otadata::resolve(&table, slot) else { continue };
        con.puts("step: verify ");
        slot_name(con, slot);
        con.put(b'\n');
        match image::verify(flash, p.offset, p.size, tick) {
            Ok(v) => {
                out.verified += 1;
                if out.chosen.is_none() {
                    out.chosen = Some(slot);
                }
                print_ok(con, slot, &v, map);
            }
            Err(e) => {
                out.failed += 1;
                con.puts("verify ");
                slot_name(con, slot);
                con.puts(": FAIL ");
                con.line(image_err(e));
            }
        }
    }
    end(con, out)
}

fn end<C: Console>(con: &mut C, out: Outcome) -> Outcome {
    con.puts("END proof: verified ");
    con.dec(out.verified as u32);
    con.puts(" failed ");
    con.dec(out.failed as u32);
    con.puts(" chosen ");
    match out.chosen {
        Some(s) => slot_name(con, s),
        None => con.puts("none"),
    }
    con.puts(" (not jumping)\n");
    out
}

fn read_table<F: FlashRead>(flash: &mut F) -> Result<Table, PartError> {
    let mut buf = [0u8; partition::TABLE_LEN];
    flash.read(partition::TABLE_OFFSET, &mut buf).map_err(|_| PartError::Empty)?;
    partition::parse(&buf)
}

fn read_otadata<F: FlashRead, C: Console>(flash: &mut F, table: &Table, con: &mut C) -> Option<[Select; 2]> {
    let p = table.otadata()?;
    let mut sel = [Select { seq: 0, state: 0, crc: 0 }; 2];
    for (i, s) in sel.iter_mut().enumerate() {
        let mut raw = [0u8; otadata::ENTRY_LEN];
        if flash.read(p.offset + i as u32 * otadata::SECTOR, &mut raw).is_err() {
            con.line("otadata: flash read failed");
            return None;
        }
        *s = Select::parse(&raw);
        con.puts("otadata ");
        con.dec(i as u32);
        con.puts(": seq ");
        con.hex32(s.seq);
        con.puts(" state ");
        con.hex32(s.state);
        con.puts(if s.crc_ok() { " crc ok" } else { " crc BAD" });
        con.line(if s.valid() { " valid" } else { " not valid" });
    }
    Some(sel)
}

fn print_table<C: Console>(con: &mut C, t: &Table) {
    for e in t.entries() {
        print_entry(con, e);
    }
    con.line(if t.md5_checked { "table md5 ok" } else { "table has no md5 entry" });
}

fn print_entry<C: Console>(con: &mut C, e: &Entry) {
    con.puts("part ");
    con.name(&e.name);
    con.puts(" type ");
    con.hex_bytes(&[e.ptype, e.subtype]);
    con.puts(" off ");
    con.hex32(e.offset);
    con.puts(" size ");
    con.hex32(e.size);
    con.put(b'\n');
}

fn print_ok<C: Console>(con: &mut C, slot: Slot, v: &Verified, map: &MemoryMap) {
    con.puts("verify ");
    slot_name(con, slot);
    con.puts(": OK len ");
    con.hex32(v.image_len);
    con.puts(" segs ");
    con.dec(v.segment_count as u32);
    con.puts(" entry ");
    con.hex32(v.entry);
    con.puts(if v.hash_appended { " hash appended+ok" } else { " no hash (checksum only)" });
    con.puts(" sha256 ");
    con.hex_bytes(&v.sha256);
    con.put(b'\n');
    for (i, s) in v.segments().iter().enumerate() {
        con.puts("  seg ");
        con.dec(i as u32);
        con.put(b' ');
        con.hex32(s.load_addr);
        con.puts(" len ");
        con.hex32(s.len);
        con.put(b' ');
        con.line(class_name(map.classify(s.load_addr, s.len)));
    }
    match image::check_loadable(v, map) {
        Ok(()) => con.line("  loadable by this stage: yes"),
        Err(LoadError::Segment { index, class }) => {
            con.puts("  loadable by this stage: NO, segment ");
            con.dec(index as u32);
            con.puts(" is ");
            con.line(class_name(class));
        }
        Err(LoadError::EntryOutsideSegments) => con.line("  loadable by this stage: NO, entry outside the segments"),
    }
}

fn slot_name<C: Console>(con: &mut C, s: Slot) {
    match s {
        Slot::Factory => con.puts("factory"),
        Slot::Ota(n) => {
            con.puts("ota_");
            con.dec(n as u32);
        }
    }
}

fn class_name(c: SegClass) -> &'static str {
    match c {
        SegClass::Ram => "ram",
        SegClass::Mapped => "flash-mapped",
        SegClass::Forbidden => "forbidden (stage window)",
        SegClass::Overflow => "overflow",
        SegClass::Outside => "outside the map",
    }
}

fn part_err(e: PartError) -> &'static str {
    match e {
        PartError::BadMagic(_) => "bad entry magic",
        PartError::TooMany => "too many entries",
        PartError::BadMd5 => "md5 mismatch",
        PartError::BadRange(_) => "bad offset or size",
        PartError::Empty => "empty table or flash read failed",
    }
}

fn image_err(e: ImageError) -> &'static str {
    match e {
        ImageError::Flash => "flash read",
        ImageError::BadMagic(0xFF) => "erased (no image)",
        ImageError::BadMagic(_) => "bad magic",
        ImageError::BadSegmentCount(_) => "bad segment count",
        ImageError::WrongChip(_) => "wrong chip id",
        ImageError::SegmentUnaligned => "segment length not a multiple of 4",
        ImageError::TooLong => "runs past the partition",
        ImageError::BadChecksum => "checksum",
        ImageError::BadHashFlag(_) => "bad hash flag",
        ImageError::BadHash => "sha256 mismatch",
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mem::Range;
    use crate::partition::tests::table_bytes;
    use crate::testutil::{build_image, VecFlash};
    use std::string::String;
    use std::vec;
    use std::vec::Vec;

    struct Buf(Vec<u8>);
    impl Console for Buf {
        fn put(&mut self, b: u8) {
            self.0.push(b);
        }
    }
    impl Buf {
        fn text(&self) -> String {
            String::from_utf8(self.0.clone()).unwrap()
        }
    }

    const RAM: [Range; 1] = [Range::new(0x4FF1_0000, 0x4FF2_8ED0)];
    const FORB: [Range; 1] = [Range::new(0x4FF2_8ED0, 0x4FF3_ABD0)];
    const MAPPED: [Range; 1] = [Range::new(0x4000_0000, 0x4C00_0000)];
    const MAP: MemoryMap = MemoryMap { ram: &RAM, forbidden: &FORB, mapped: &MAPPED };

    fn otadata_sector(seq: u32, state: u32) -> Vec<u8> {
        let mut b = vec![0xFFu8; 32];
        b[0..4].copy_from_slice(&seq.to_le_bytes());
        b[24..28].copy_from_slice(&state.to_le_bytes());
        b[28..32].copy_from_slice(&crate::crc::crc32_le(u32::MAX, &seq.to_le_bytes()).to_le_bytes());
        b
    }

    /// ota_0 holds a flash-mapped (IDF style) image, ota_1 a RAM one; otadata selects ota_0.
    fn badge() -> VecFlash {
        let v2 = build_image(0x4200_0020, &[(0x4200_0020, vec![1u8; 64]), (0x3C00_0020, vec![2u8; 32])], true);
        let v3 = build_image(0x4FF1_0000, &[(0x4FF1_0000, vec![3u8; 64])], true);
        let t = table_bytes(true);
        let od = otadata_sector(1, 2);
        VecFlash::sparse(0x80_0000, &[(0x8000, &t), (0x11000, &od), (0x20000, &v2), (0x42_0000, &v3)])
    }

    #[test]
    fn reports_both_slots_and_never_jumps() {
        let mut f = badge();
        let mut c = Buf(Vec::new());
        let o = run(&mut f, &mut c, &MAP, &mut || {});
        let t = c.text();
        assert_eq!(o, Outcome { chosen: Some(Slot::Ota(0)), verified: 2, failed: 0 });
        assert!(t.contains("part ota_0 type 0010 off 0x00020000"), "{t}");
        assert!(t.contains("otadata 0: seq 0x00000001 state 0x00000002 crc ok valid"), "{t}");
        assert!(t.contains("order: ota_0 ota_1 (otadata copy 0)"), "{t}");
        assert!(t.contains("verify ota_0: OK"), "{t}");
        assert!(t.contains("loadable by this stage: NO, segment 0 is flash-mapped"), "{t}");
        assert!(t.contains("verify ota_1: OK"), "{t}");
        assert!(t.contains("loadable by this stage: yes"), "{t}");
        assert!(t.trim_end().ends_with("END proof: verified 2 failed 0 chosen ota_0 (not jumping)"), "{t}");
    }

    #[test]
    fn falls_back_when_the_primary_is_corrupt() {
        // ota_0 with a flipped byte in its segment data
        let mut f = {
            let v2 = build_image(0x4200_0020, &[(0x4200_0020, vec![1u8; 64])], true);
            let mut bad = v2.clone();
            bad[40] ^= 0xFF;
            let v3 = build_image(0x4FF1_0000, &[(0x4FF1_0000, vec![3u8; 64])], true);
            let t = table_bytes(true);
            let od = otadata_sector(1, 2);
            VecFlash::sparse(0x80_0000, &[(0x8000, &t), (0x11000, &od), (0x20000, &bad), (0x42_0000, &v3)])
        };
        let mut c = Buf(Vec::new());
        let o = run(&mut f, &mut c, &MAP, &mut || {});
        assert_eq!(o, Outcome { chosen: Some(Slot::Ota(1)), verified: 1, failed: 1 });
        assert!(c.text().contains("verify ota_0: FAIL checksum"), "{}", c.text());
    }

    #[test]
    fn erased_flash_fails_cleanly() {
        let mut f = VecFlash::sparse(0x10000, &[]);
        let mut c = Buf(Vec::new());
        let o = run(&mut f, &mut c, &MAP, &mut || {});
        assert_eq!(o.chosen, None);
        assert!(c.text().contains("FAIL partition table"), "{}", c.text());
    }
}
