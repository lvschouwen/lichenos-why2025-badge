//! CPU-kernel helpers: straight-line code for the instruction-fetch test, a bitwise CRC32 for the data-placement test, and the
//! conversions the kernel results go through.

/// `add t0, t0, t1` (rd = rs1 = t0 = x5, rs2 = t1 = x6): a dependent one-cycle instruction that needs no operand setup.
pub const ADD_T0_T0_T1: u32 = 0x0062_82B3;
/// `ret` = `jalr x0, 0(ra)`.
pub const RET: u32 = 0x0000_8067;

/// Fill `code` with `adds` copies of the add and a `ret`; returns the number of words written (`adds + 1`), or `None` if it does not
/// fit. The code is position independent (no branch, no address), so it runs wherever it is copied.
pub fn straight_line(code: &mut [u32], adds: usize) -> Option<usize> {
    if adds + 1 > code.len() {
        return None;
    }
    code[..adds].fill(ADD_T0_T0_T1);
    code[adds] = RET;
    Some(adds + 1)
}

/// CRC-32 (IEEE, reflected, bit at a time: no table, so the loop is CPU-bound and the only memory it touches is the buffer).
pub fn crc32_update(mut crc: u32, data: &[u8]) -> u32 {
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    crc
}
pub fn crc32(data: &[u8]) -> u32 {
    !crc32_update(!0, data)
}

/// A value in hundredths: `num / den` (0 for a zero denominator), e.g. cycles per instruction from two counters.
pub fn ratio_x100(num: u64, den: u64) -> u64 {
    if den == 0 {
        return 0;
    }
    (num as u128 * 100 / den as u128) as u64
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec;

    #[test]
    fn the_instruction_words_are_the_riscv_encodings() {
        // add rd=5 rs1=5 rs2=6: funct7 0 | rs2 | rs1 | funct3 0 | rd | opcode 0x33
        assert_eq!(ADD_T0_T0_T1, 6 << 20 | 5 << 15 | 5 << 7 | 0x33);
        // jalr x0, 0(x1)
        assert_eq!(RET, 1 << 15 | 0x67);
    }

    #[test]
    fn straight_line_code_ends_in_ret() {
        let mut c = vec![0u32; 16];
        assert_eq!(straight_line(&mut c, 15), Some(16));
        assert!(c[..15].iter().all(|&w| w == ADD_T0_T0_T1));
        assert_eq!(c[15], RET);
        assert_eq!(straight_line(&mut c, 16), None); // no room for the ret
        let mut tiny = [0u32; 1];
        assert_eq!(straight_line(&mut tiny, 0), Some(1));
        assert_eq!(tiny[0], RET);
    }

    #[test]
    fn crc32_matches_the_check_value() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32(b""), 0);
        // chunked updates give the same answer as one call
        let whole = crc32(b"the quick brown fox");
        let part = !crc32_update(crc32_update(!0, b"the quick "), b"brown fox");
        assert_eq!(whole, part);
    }

    #[test]
    fn ratios() {
        assert_eq!(ratio_x100(300, 100), 300);
        assert_eq!(ratio_x100(1, 3), 33);
        assert_eq!(ratio_x100(5, 0), 0);
    }
}
