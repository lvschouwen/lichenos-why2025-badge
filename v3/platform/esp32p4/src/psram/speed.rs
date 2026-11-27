//! PSRAM speeds. Only 120 MHz (the default and the floor) is reachable without the `bench-experiments` feature.

/// A PSRAM speed: MPLL, bus divider (MPLL / PSRAM, even), latency codes and dummy bits (APS256XXN latency tables: LC5/WLC5 up
/// to 133 MHz, LC6/WLC6 up to 166 MHz, LC7/WLC7 up to 200 MHz; IDF's own 200 MHz setting is LC7/WLC7).
#[derive(Clone, Copy, Debug)]
pub struct Speed {
    pub mhz: u32,
    pub mpll: u32,
    /// MR0 [4:2], read latency code field: cycles = 2 * field + 6.
    pub rd_lat: u8,
    /// MR4 [7:5], write latency code field (010b = WLC5, 110b = WLC6, 001b = WLC7).
    pub wr_lat: u8,
    pub rd_dummy: u32,
    pub rd_reg_dummy: u32,
    pub wr_dummy: u32,
}
/// The default and floor: 120 MHz = MPLL 480 / 4.
pub const SPEED_120: Speed = Speed {
    mhz: 120,
    mpll: 480,
    rd_lat: 2,
    wr_lat: 2,
    rd_dummy: 2 * (10 - 1),
    rd_reg_dummy: 2 * (5 - 1),
    wr_dummy: 2 * (5 - 1),
};
/// Experiment (#645): 160 MHz = MPLL 320 / 2 (an even divider; 480 / 3 would give the DDR bus a 33 % duty clock), LC6 / WLC6.
#[cfg(feature = "bench-experiments")]
pub const SPEED_160: Speed = Speed {
    mhz: 160,
    mpll: 320,
    rd_lat: 3,
    wr_lat: 6,
    rd_dummy: 2 * (12 - 1),
    rd_reg_dummy: 2 * (6 - 1),
    wr_dummy: 2 * (6 - 1),
};

/// `SPIMEM2 + 0x40` fields the AXI side needs for a speed: read dummy [11:6] = bits - 1 (enable bit 4 stays set from
/// `regs::CONFIG_FOR_PSRAM`), write dummy [27:22] = bits - 1.
pub const fn axi_dummy_fields(sp: &Speed) -> (u32, u32) {
    ((sp.rd_dummy - 1) << 6, (sp.wr_dummy - 1) << 22)
}

/// `(rd_reg_dummy, MR0 read latency field)` of every speed this build can program: what `mspi::pick_reg_dummy` chooses from.
/// One entry (120 MHz) in the default build, so nothing is probed there.
pub const KNOWN_REG_DUMMIES: &[(u32, u8)] = &[
    (SPEED_120.rd_reg_dummy, SPEED_120.rd_lat),
    #[cfg(feature = "bench-experiments")]
    (SPEED_160.rd_reg_dummy, SPEED_160.rd_lat),
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn axi_fields_of_120_match_the_register_table() {
        // regs::CONFIG_FOR_PSRAM: read dummy 18 bits = 0x450 (field 17 << 6, enable bit 4 separate), write dummy 8 bits = 0x01C0_0008
        let (rd, wr) = axi_dummy_fields(&SPEED_120);
        assert_eq!(rd, 0x440);
        assert_eq!(wr, 0x01C0_0000);
    }
}
