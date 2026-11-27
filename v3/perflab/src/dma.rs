//! AXI-GDMA burst size: `IN_CONF0` (RX) [6:4] and `OUT_CONF0` (TX) [7:5], code = log2(bytes) - 3 (8 to 128 B; the PSRAM controller
//! accepts at most 128 B). The platform's `dma::init` sets 32 B; the probe changes it afterwards.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Burst {
    B32,
    B64,
    B128,
}
impl Burst {
    pub const fn from_bytes(b: u32) -> Option<Burst> {
        match b {
            32 => Some(Burst::B32),
            64 => Some(Burst::B64),
            128 => Some(Burst::B128),
            _ => None,
        }
    }
    pub const fn bytes(self) -> u32 {
        match self {
            Burst::B32 => 32,
            Burst::B64 => 64,
            Burst::B128 => 128,
        }
    }
    const fn code(self) -> u32 {
        self.bytes().trailing_zeros() - 3
    }
    /// `RX_CONF0` field value (mask `RX_MASK`).
    pub const fn rx(self) -> u32 {
        self.code() << 4
    }
    /// `TX_CONF0` field value (mask `TX_MASK`).
    pub const fn tx(self) -> u32 {
        self.code() << 5
    }
}
pub const RX_MASK: u32 = 0x70;
pub const TX_MASK: u32 = 0xE0;

/// Number of transfers of `chunk` bytes that cover `total` (the last one may be short only if `total` is not a multiple).
pub const fn transfers(total: usize, chunk: usize) -> usize {
    total.div_ceil(chunk)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fields_match_the_idf_encoding() {
        // platform dma::init writes 0x20 / 0x40 for 32 B; 64 B and 128 B are the next two codes
        assert_eq!((Burst::B32.rx(), Burst::B32.tx()), (0x20, 0x40));
        assert_eq!((Burst::B64.rx(), Burst::B64.tx()), (0x30, 0x60));
        assert_eq!((Burst::B128.rx(), Burst::B128.tx()), (0x40, 0x80));
        for b in [Burst::B32, Burst::B64, Burst::B128] {
            assert_eq!(b.rx() & !RX_MASK, 0);
            assert_eq!(b.tx() & !TX_MASK, 0);
            assert_eq!(Burst::from_bytes(b.bytes()), Some(b));
        }
    }

    #[test]
    fn only_32_64_128() {
        for b in [0, 8, 16, 48, 96, 256] {
            assert_eq!(Burst::from_bytes(b), None);
        }
    }

    #[test]
    fn transfer_count() {
        assert_eq!(transfers(32 << 20, 16 << 10), 2048);
        assert_eq!(transfers(10, 4), 3);
        assert_eq!(transfers(0, 4), 0);
    }
}
