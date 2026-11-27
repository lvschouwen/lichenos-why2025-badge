//! The AP Memory HEX PSRAM device: command transactions and mode registers, ported from ESP-IDF v6.1
//! `esp_psram/device/esp_psram_impl_ap_hex.c` with the Lichen patches (`tools/idf-patches` 0001 120 MHz, 0004 refresh).
//!
//! Transactions go through the P4 mask ROM's user-command helpers, exactly as IDF's `psram_ctrlr_ll_common_transaction`
//! does (`esp_rom_spi_set_op_mode`, `esp_rom_spi_cmd_config`, `esp_rom_spi_cmd_start`): they drive the FIFO side of the
//! controller (SPIMEM3). ROM addresses are those of the rev < 3 ROM (esp32p4.rom.eco0_4.ld), the one v2 is built for.
use core::mem::transmute;

const ROM_SPI_CMD_CONFIG: usize = 0x4FC0_0108;
const ROM_SPI_CMD_START: usize = 0x4FC0_010C;
const ROM_SPI_SET_OP_MODE: usize = 0x4FC0_0110;
const ROM_DELAY_US: usize = 0x4FC0_003C; // ets_delay_us
const OPI_DTR_MODE: i32 = 7; // ESP_ROM_SPIFLASH_OPI_DTR_MODE
/// SPI1 side of the PSRAM controller (PSRAM_CTRLR_LL_MSPI_ID_3).
const MSPI_ID_PERI: i32 = 3;
const CS_MASK: u8 = 1 << 1;

pub const SYNC_READ: u32 = 0x0000;
pub const SYNC_WRITE: u32 = 0x8080;
pub const REG_READ: u32 = 0x4040;
pub const REG_WRITE: u32 = 0xC0C0;
pub const CMD_BITLEN: u32 = 16;
pub const ADDR_BITLEN: u32 = 32;
/// 120 MHz (and 80 MHz) latency codes: AP_HEX_PSRAM_*: LC5 / WLC5, i.e. mode-register fields 2 and 2.
pub const RD_DUMMY_BITLEN: u32 = 2 * (10 - 1);
pub const RD_REG_DUMMY_BITLEN: u32 = 2 * (5 - 1);
pub const WR_DUMMY_BITLEN: u32 = 2 * (5 - 1);
pub const RD_LATENCY: u8 = 2;
pub const WR_LATENCY: u8 = 2;
pub const FIFO_MAX_BYTES: usize = 64;
pub const REF_DATA: u32 = 0x5a6b_7c8d;

/// `esp_rom_spi_cmd_t`
#[repr(C)]
struct RomSpiCmd {
    cmd: u16,
    cmd_bitlen: u16,
    addr: *const u32,
    addr_bitlen: u32,
    tx: *const u32,
    tx_bitlen: u32,
    rx: *mut u32,
    rx_bitlen: u32,
    dummy_bitlen: u32,
}

/// Microsecond busy wait of the ROM (right for the clock the ROM left).
pub fn delay_us(us: u32) {
    // SAFETY: ROM `void ets_delay_us(uint32_t)`, no side effects beyond the wait.
    let f: extern "C" fn(u32) = unsafe { transmute(ROM_DELAY_US) };
    f(us)
}

/// One PSRAM transaction on the FIFO side (`psram_ctrlr_ll_common_transaction`). `tx` and `rx` are word buffers; `*_bits`
/// say how much of them is sent or received (at most 64 bytes).
///
/// # Safety
/// The PSRAM controller must be clocked and configured (`init` order); calls ROM code.
pub unsafe fn xact(cmd: u32, addr: u32, dummy_bits: u32, tx: Option<(&[u32], u32)>, rx: Option<(&mut [u32], u32)>) {
    let (txp, txb) = tx.map_or((core::ptr::null(), 0), |(b, n)| (b.as_ptr(), n));
    let (rxp, rxb) = match rx {
        Some((b, n)) => (b.as_mut_ptr(), n),
        None => (core::ptr::null_mut(), 0),
    };
    let set_mode: extern "C" fn(i32, i32) = transmute(ROM_SPI_SET_OP_MODE);
    let cmd_config: extern "C" fn(i32, *const RomSpiCmd) = transmute(ROM_SPI_CMD_CONFIG);
    let cmd_start: extern "C" fn(i32, *mut u8, u16, u8, bool) = transmute(ROM_SPI_CMD_START);
    set_mode(MSPI_ID_PERI, OPI_DTR_MODE);
    let conf = RomSpiCmd {
        cmd: cmd as u16,
        cmd_bitlen: CMD_BITLEN as u16,
        addr: &addr,
        addr_bitlen: ADDR_BITLEN,
        tx: txp,
        tx_bitlen: txb,
        rx: rxp,
        rx_bitlen: rxb,
        dummy_bitlen: dummy_bits,
    };
    cmd_config(MSPI_ID_PERI, &conf);
    cmd_start(MSPI_ID_PERI, rxp as *mut u8, (rxb / 8) as u16, CS_MASK, false);
}

/// Mode registers MR0..MR8 as IDF's `hex_psram_mode_reg_t` lays them out, one byte each.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct ModeRegs {
    pub mr0: u8,
    pub mr1: u8,
    pub mr2: u8,
    pub mr3: u8,
    pub mr4: u8,
    pub mr8: u8,
}
impl ModeRegs {
    pub fn vendor_id(&self) -> u8 {
        self.mr1 & 0x1F
    }
    /// MR2 density field: 1 = 32 Mbit, 3 = 64, 5 = 128, 7 = 256.
    pub fn density(&self) -> u8 {
        self.mr2 & 7
    }
    pub fn known_good_die(&self) -> u8 {
        self.mr2 >> 5
    }
    /// MR4 RF, the refresh rate: 0 = always 4x, 1 = 1x when the temperature allows, 3 = 0.5x.
    pub fn refresh(&self) -> u8 {
        (self.mr4 >> 3) & 3
    }
    pub fn size_bytes(&self) -> u32 {
        match self.density() {
            1 => 4 << 20,
            3 => 8 << 20,
            5 => 16 << 20,
            7 => 32 << 20,
            6 => 64 << 20,
            _ => 0,
        }
    }
}

/// MR0 as IDF writes it: drive strength 0, read latency `RD_LATENCY`, fixed latency, the other bits kept.
pub const fn mr0_value(read_back: u8) -> u8 {
    (read_back & 0xC0) | (RD_LATENCY << 2) | (1 << 5)
}
/// MR4 as IDF (with Lichen patch 0004) writes it: write latency, refresh 0 = always 4x, PASR kept.
pub const fn mr4_value(read_back: u8) -> u8 {
    (read_back & 0x07) | (WR_LATENCY << 5)
}
/// MR8 as IDF writes it: burst length 3 (2 KB), hybrid burst off, row-boundary crossing on, x16; reserved bits kept.
pub const fn mr8_value(read_back: u8) -> u8 {
    (read_back & 0xB0) | 3 | (1 << 3) | (1 << 6)
}

fn byte(words: &[u32], i: usize) -> u8 {
    (words[i / 4] >> (8 * (i % 4))) as u8
}
fn word2(lo: u8, hi: u8) -> [u32; 1] {
    [lo as u32 | ((hi as u32) << 8)]
}

/// `s_init_psram_mode_reg`: read MR0..MR5, write MR0/1, MR4/5 and MR8 as IDF does, with the second byte of each 16-bit
/// write exactly as IDF sends it (MR1 and MR5 as read; for MR8 the byte after it is zero here, undefined stack in IDF).
///
/// # Safety
/// As `xact`.
pub unsafe fn init_mode_regs() {
    let mut r = [0u32; 1];
    xact(REG_READ, 0, RD_REG_DUMMY_BITLEN, None, Some((&mut r, 16)));
    let (m0, m1) = (byte(&r, 0), byte(&r, 1));
    xact(REG_READ, 4, RD_REG_DUMMY_BITLEN, None, Some((&mut r, 16)));
    let (m4, m5) = (byte(&r, 0), byte(&r, 1));
    xact(REG_WRITE, 0, 0, Some((&word2(mr0_value(m0), m1), 16)), None);
    xact(REG_WRITE, 4, 0, Some((&word2(mr4_value(m4), m5), 16)), None);
    xact(REG_READ, 8, RD_REG_DUMMY_BITLEN, None, Some((&mut r, 8)));
    let m8 = byte(&r, 0);
    xact(REG_WRITE, 8, 0, Some((&word2(mr8_value(m8), 0), 16)), None);
}

/// `s_get_psram_mode_reg`: MR0..MR4 and MR8 as the chip reports them now.
///
/// # Safety
/// As `xact`.
pub unsafe fn read_mode_regs() -> ModeRegs {
    let mut r = [0u32; 1];
    let mut m = ModeRegs::default();
    xact(REG_READ, 0, RD_REG_DUMMY_BITLEN, None, Some((&mut r, 16)));
    (m.mr0, m.mr1) = (byte(&r, 0), byte(&r, 1));
    xact(REG_READ, 2, RD_REG_DUMMY_BITLEN, None, Some((&mut r, 16)));
    (m.mr2, m.mr3) = (byte(&r, 0), byte(&r, 1));
    xact(REG_READ, 4, RD_REG_DUMMY_BITLEN, None, Some((&mut r, 8)));
    m.mr4 = byte(&r, 0);
    xact(REG_READ, 8, RD_REG_DUMMY_BITLEN, None, Some((&mut r, 8)));
    m.mr8 = byte(&r, 0);
    m
}

/// `s_check_psram_connected`: write `REF_DATA` at address 0 and read it back.
///
/// # Safety
/// As `xact`.
pub unsafe fn check_connected() -> bool {
    let w = [REF_DATA];
    xact(SYNC_WRITE, 0, WR_DUMMY_BITLEN, Some((&w, 32)), None);
    let mut r = [0u32; 1];
    xact(SYNC_READ, 0, RD_DUMMY_BITLEN, None, Some((&mut r, 32)));
    r[0] == REF_DATA
}

/// Write `data` (a whole number of words) at `addr` in FIFO-sized pieces (`mspi_timing_config_psram_write_data`).
///
/// # Safety
/// As `xact`.
pub unsafe fn write_data(data: &[u32], mut addr: u32) {
    for chunk in data.chunks(FIFO_MAX_BYTES / 4) {
        xact(SYNC_WRITE, addr, WR_DUMMY_BITLEN, Some((chunk, chunk.len() as u32 * 32)), None);
        addr += chunk.len() as u32 * 4;
    }
}

/// Read `out.len()` words from `addr` (`mspi_timing_config_psram_read_data`).
///
/// # Safety
/// As `xact`.
pub unsafe fn read_data(out: &mut [u32], mut addr: u32) {
    for chunk in out.chunks_mut(FIFO_MAX_BYTES / 4) {
        let bits = chunk.len() as u32 * 32;
        xact(SYNC_READ, addr, RD_DUMMY_BITLEN, None, Some((chunk, bits)));
        addr += bits / 8;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mode_register_values_follow_idf() {
        // MR0: drive strength 0, read latency field 2 at [4:2], LT fixed (bit 5); bits 6 and 7 kept
        assert_eq!(mr0_value(0x00), 0b0010_1000);
        assert_eq!(mr0_value(0xC3), 0b1110_1000);
        // MR4: write latency field 2 at [7:5], RF = 0 (4x), PASR kept
        assert_eq!(mr4_value(0x00), 0x40);
        assert_eq!(mr4_value(0xFF), 0x47);
        // MR8: BL 3, BT 0, RBX 1, X16 1; reserved bits 4, 5 and 7 kept
        assert_eq!(mr8_value(0x00), 0b0100_1011);
        assert_eq!(mr8_value(0xFF), 0b1111_1011);
    }

    #[test]
    fn density_decode() {
        let m = ModeRegs { mr2: 0b1100_0111, ..Default::default() };
        assert_eq!((m.density(), m.known_good_die(), m.size_bytes()), (7, 6, 32 << 20));
        assert_eq!(ModeRegs { mr4: 0b0001_1000, ..Default::default() }.refresh(), 3);
    }
}
