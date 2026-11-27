//! CRC-32 as the ESP ROM's `esp_rom_crc32_le`, which the IDF otadata CRC uses (`bootloader_common_ota_select_crc`
//! calls it with `UINT32_MAX`).

/// `esp_rom_crc32_le(crc, data)`: reflected polynomial 0xEDB88320, the running value is inverted on entry and exit.
pub fn crc32_le(crc: u32, data: &[u8]) -> u32 {
    let mut c = !crc;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = (c >> 1) ^ (0xEDB8_8320 & (c & 1).wrapping_neg());
        }
    }
    !c
}

#[cfg(test)]
mod tests {
    use super::*;

    /// IDF documents ota_seq 1 with crc 0x4743989A (esp_ota_ops tests); pins the init and the inversion.
    #[test]
    fn otadata_crc_of_seq_1() {
        assert_eq!(crc32_le(u32::MAX, &1u32.to_le_bytes()), 0x4743_989A);
    }

    #[test]
    fn matches_zlib_crc32_with_zero_start() {
        // zlib crc32("123456789") = 0xCBF43926 = crc32_le(0, ...)
        assert_eq!(crc32_le(0, b"123456789"), 0xCBF4_3926);
    }
}
