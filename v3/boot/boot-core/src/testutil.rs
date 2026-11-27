//! Test helpers: a Vec-backed flash and an ESP image builder written from the format description (not from the
//! parser), so a parser bug cannot hide behind a builder with the same bug. `check.sh` also verifies a real
//! `esptool elf2image` output with `examples/verify.rs`.
use crate::flash::{FlashError, FlashRead};
use sha2::{Digest, Sha256};
use std::vec::Vec;

pub struct VecFlash {
    base: u32,
    data: Vec<u8>,
}

impl VecFlash {
    /// `data` sits at flash offset `base`; every other offset reads as an error.
    pub fn at(base: u32, data: Vec<u8>) -> Self {
        VecFlash { base, data }
    }
    /// Several regions, the gaps filled with 0xFF (erased flash), `size` bytes in all.
    pub fn sparse(size: usize, regions: &[(u32, &[u8])]) -> Self {
        let mut data = std::vec![0xFFu8; size];
        for (o, d) in regions {
            data[*o as usize..*o as usize + d.len()].copy_from_slice(d);
        }
        VecFlash { base: 0, data }
    }
}

impl FlashRead for VecFlash {
    fn read(&mut self, offset: u32, buf: &mut [u8]) -> Result<(), FlashError> {
        let o = offset.checked_sub(self.base).ok_or(FlashError)? as usize;
        let end = o.checked_add(buf.len()).ok_or(FlashError)?;
        buf.copy_from_slice(self.data.get(o..end).ok_or(FlashError)?);
        Ok(())
    }
}

/// An ESP32-P4 image. Segment data lengths must be multiples of 4.
pub fn build_image(entry: u32, segs: &[(u32, Vec<u8>)], hash: bool) -> Vec<u8> {
    let mut b = std::vec![0u8; 24];
    b[0] = 0xE9;
    b[1] = segs.len() as u8;
    b[4..8].copy_from_slice(&entry.to_le_bytes());
    b[12] = 18;
    b[23] = hash as u8;
    let mut xor = 0xEFu8;
    for (addr, data) in segs {
        b.extend(addr.to_le_bytes());
        b.extend((data.len() as u32).to_le_bytes());
        b.extend(data);
        for x in data {
            xor ^= x;
        }
    }
    while b.len() % 16 != 15 {
        b.push(0);
    }
    b.push(xor);
    if hash {
        let d = Sha256::digest(&b);
        b.extend(d);
    }
    b
}
