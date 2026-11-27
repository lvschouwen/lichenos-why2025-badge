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
use core::sync::atomic::{AtomicU32, Ordering::Relaxed};
static RD_DUMMY: AtomicU32 = AtomicU32::new(RD_DUMMY_BITLEN);
static RD_REG_DUMMY: AtomicU32 = AtomicU32::new(RD_REG_DUMMY_BITLEN);
static WR_DUMMY: AtomicU32 = AtomicU32::new(WR_DUMMY_BITLEN);
/// Dummy bits of the FIFO-path transactions for the speed the chip is programmed for (set after its mode registers are written) (the constants above are the 80 and 120 MHz values;
/// `psram::Speed` sets the others, e.g. 160 MHz: LC6 / WLC6, 2 * (12 - 1), 2 * (6 - 1), 2 * (6 - 1)).
pub fn set_timing(rd: u32, rd_reg: u32, wr: u32) {
    RD_DUMMY.store(rd, Relaxed);
    RD_REG_DUMMY.store(rd_reg, Relaxed);
    WR_DUMMY.store(wr, Relaxed);
}
fn rd_dummy() -> u32 {
    RD_DUMMY.load(Relaxed)
}
fn rd_reg_dummy() -> u32 {
    RD_REG_DUMMY.load(Relaxed)
}
fn wr_dummy() -> u32 {
    WR_DUMMY.load(Relaxed)
}
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
    if let Some((b, n)) = &tx {
        assert!(
            *n as usize <= FIFO_MAX_BYTES * 8 && *n as usize <= b.len() * 32,
            "tx bits exceed the FIFO or the buffer"
        );
    }
    if let Some((b, n)) = &rx {
        assert!(
            *n as usize <= FIFO_MAX_BYTES * 8 && *n as usize <= b.len() * 32,
            "rx bits exceed the FIFO or the buffer"
        );
    }
    assert!(dummy_bits <= 255);
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
    /// Whether the registers read back are what `init_mode_regs(rl, wl)` writes on the expected part (vendor 0xD, 256 Mbit):
    /// read latency field and fixed latency (MR0), write latency, refresh 4x and PASR 0 (MR4 exact), burst, row crossing and
    /// x16 (MR8). A chip still at another speed's latency, or a link that reads garbage, fails this.
    pub fn matches_init(&self, rl: u8, wl: u8) -> bool {
        self.vendor_id() == 0xD
            && self.density() == 7
            && (self.mr0 >> 2) & 7 == rl
            && self.mr0 & (1 << 5) != 0
            && self.mr4 == mr4_value(0, wl)
            && self.mr8 & 0x4B == 0x4B
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

/// The read-register dummy that matches the chip's CURRENT latency. After a failed speed change the chip may still be at the
/// old latency while `set_timing` already holds the new dummies, and a register read with the wrong dummy returns a neighbouring
/// register shifted into place (MR1 = 0x8D looks like latency field 3, MR8 = 0x4B like field 2). Each candidate is
/// `(rd_reg_dummy, MR0 read latency field)` of a known speed; `read_regs(d)` returns `(MR0, MR1)` of one 16-bit read made with
/// dummy `d`. A dummy counts as consistent only if the pair looks like the real registers: MR1's vendor ID is 0xD, MR0's latency
/// field is that speed's own, and MR0's fixed-latency bit 5 is set. The current dummy is tried first; none consistent keeps the
/// current one. With a single candidate nothing is read (the default 120 MHz build behaves exactly as before).
pub fn pick_reg_dummy(candidates: &[(u32, u8)], current: u32, read_regs: &mut dyn FnMut(u32) -> (u8, u8)) -> u32 {
    if candidates.len() < 2 {
        return current;
    }
    let consistent = |d: u32, read: &mut dyn FnMut(u32) -> (u8, u8)| {
        let (mr0, mr1) = read(d);
        mr1 & 0x1F == 0xD && mr0 & (1 << 5) != 0 && candidates.iter().any(|&(cd, rl)| cd == d && rl == (mr0 >> 2) & 7)
    };
    if consistent(current, read_regs) {
        return current;
    }
    for &(d, _) in candidates {
        if d != current && consistent(d, read_regs) {
            return d;
        }
    }
    current
}

/// Switch the read-register dummy alone (see `pick_reg_dummy`).
pub fn set_reg_dummy(bits: u32) {
    RD_REG_DUMMY.store(bits, Relaxed);
}

/// The current read-register dummy.
pub fn reg_dummy() -> u32 {
    rd_reg_dummy()
}

/// MR0 and MR1 from one 16-bit register read with an explicit dummy.
///
/// With a dummy that does not match the chip's latency the ROM `cmd_start` still waits for data that never arrives in the
/// expected cycle; it has no timeout, so a badly wrong dummy could in principle hang here. The known speeds' dummies differ by
/// a few cycles only, which has answered with shifted data on the bench so far.
///
/// # Safety
/// As `xact`.
pub unsafe fn read_regs_with(dummy: u32) -> (u8, u8) {
    let mut r = [0u32; 1];
    xact(REG_READ, 0, dummy, None, Some((&mut r, 16)));
    (byte(&r, 0), byte(&r, 1))
}

static MODE_PROGRAMMED: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(false);
/// Whether this boot has already written the chip's mode registers (`init_mode_regs` ran). Before that the chip has its power-on
/// MR0 and the dummies are right by construction, so there is nothing to probe.
pub fn mode_programmed() -> bool {
    MODE_PROGRAMMED.load(Relaxed)
}

/// MR0 as IDF writes it: drive strength 0, read latency `RD_LATENCY`, fixed latency, the other bits kept.
pub const fn mr0_value(read_back: u8, rl: u8) -> u8 {
    (read_back & 0xC0) | (rl << 2) | (1 << 5)
}
/// MR4 as IDF (with Lichen patch 0004) writes it: write latency, refresh 0 = always 4x, PASR 0 (the whole array keeps
/// refreshing, whatever a read made with the wrong latency returned).
pub const fn mr4_value(_read_back: u8, wl: u8) -> u8 {
    wl << 5
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
pub unsafe fn init_mode_regs(rl: u8, wl: u8) {
    let mut r = [0u32; 1];
    xact(REG_READ, 0, rd_reg_dummy(), None, Some((&mut r, 16)));
    let (m0, m1) = (byte(&r, 0), byte(&r, 1));
    xact(REG_READ, 4, rd_reg_dummy(), None, Some((&mut r, 16)));
    let (m4, m5) = (byte(&r, 0), byte(&r, 1));
    xact(REG_WRITE, 0, 0, Some((&word2(mr0_value(m0, rl), m1), 16)), None);
    xact(REG_WRITE, 4, 0, Some((&word2(mr4_value(m4, wl), m5), 16)), None);
    xact(REG_READ, 8, rd_reg_dummy(), None, Some((&mut r, 8)));
    let m8 = byte(&r, 0);
    xact(REG_WRITE, 8, 0, Some((&word2(mr8_value(m8), 0), 16)), None);
    MODE_PROGRAMMED.store(true, Relaxed);
}

/// `s_get_psram_mode_reg`: MR0..MR4 and MR8 as the chip reports them now.
///
/// # Safety
/// As `xact`.
pub unsafe fn read_mode_regs() -> ModeRegs {
    let mut r = [0u32; 1];
    let mut m = ModeRegs::default();
    xact(REG_READ, 0, rd_reg_dummy(), None, Some((&mut r, 16)));
    (m.mr0, m.mr1) = (byte(&r, 0), byte(&r, 1));
    xact(REG_READ, 2, rd_reg_dummy(), None, Some((&mut r, 16)));
    (m.mr2, m.mr3) = (byte(&r, 0), byte(&r, 1));
    xact(REG_READ, 4, rd_reg_dummy(), None, Some((&mut r, 8)));
    m.mr4 = byte(&r, 0);
    xact(REG_READ, 8, rd_reg_dummy(), None, Some((&mut r, 8)));
    m.mr8 = byte(&r, 0);
    m
}

/// `s_check_psram_connected`: write `REF_DATA` at address 0 and read it back.
///
/// # Safety
/// As `xact`.
pub unsafe fn check_connected() -> bool {
    let w = [REF_DATA];
    xact(SYNC_WRITE, 0, wr_dummy(), Some((&w, 32)), None);
    let mut r = [0u32; 1];
    xact(SYNC_READ, 0, rd_dummy(), None, Some((&mut r, 32)));
    r[0] == REF_DATA
}

/// Write `data` (a whole number of words) at `addr` in FIFO-sized pieces (`mspi_timing_config_psram_write_data`).
///
/// # Safety
/// As `xact`.
pub unsafe fn write_data(data: &[u32], mut addr: u32) {
    for chunk in data.chunks(FIFO_MAX_BYTES / 4) {
        xact(SYNC_WRITE, addr, wr_dummy(), Some((chunk, chunk.len() as u32 * 32)), None);
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
        xact(SYNC_READ, addr, rd_dummy(), None, Some((chunk, bits)));
        addr += bits / 8;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mode_register_values_follow_idf() {
        // MR0: drive strength 0, read latency field 2 at [4:2], LT fixed (bit 5); bits 6 and 7 kept
        assert_eq!(mr0_value(0x00, RD_LATENCY), 0b0010_1000);
        assert_eq!(mr0_value(0xC3, RD_LATENCY), 0b1110_1000);
        // MR4: write latency field 2 at [7:5], RF = 0 (4x), PASR 0
        assert_eq!(mr4_value(0x00, WR_LATENCY), 0x40);
        assert_eq!(mr4_value(0xFF, WR_LATENCY), 0x40); // PASR is always written 0
                                                       // 160 MHz: read latency field 3 (LC6), write latency field 6 (110b = WLC6)
        assert_eq!(mr0_value(0x00, 3), 0b0010_1100);
        assert_eq!(mr4_value(0x00, 6), 0xC0);
        // MR8: BL 3, BT 0, RBX 1, X16 1; reserved bits 4, 5 and 7 kept
        assert_eq!(mr8_value(0x00), 0b0100_1011);
        assert_eq!(mr8_value(0xFF), 0b1111_1011);
    }

    #[test]
    fn mode_readback_check() {
        let ok = ModeRegs { mr0: 0x28, mr1: 0x8d, mr2: 0xdf, mr3: 0xa0, mr4: 0x40, mr8: 0x4b };
        assert!(ok.matches_init(2, 2));
        assert!(!ok.matches_init(3, 6)); // chip still at LC5 when LC6 was written
        assert!(!ModeRegs { mr4: 0x47, ..ok }.matches_init(2, 2)); // PASR not 0
        assert!(!ModeRegs { mr1: 0x00, ..ok }.matches_init(2, 2)); // wrong vendor
        assert!(!ModeRegs { mr2: 0xd5, ..ok }.matches_init(2, 2)); // wrong density
        assert!(!ModeRegs { mr8: 0x00, ..ok }.matches_init(2, 2));
    }

    /// A chip at the given MR0 latency field. A read with that field's own dummy returns (MR0, MR1) as they are; any other dummy
    /// shifts a neighbouring register into place: toward a longer dummy MR1 (0x8D) and MR2 (0xDF), toward a shorter one MR8
    /// (0x4B) and a zero byte.
    fn fake_chip(field: u8, dummy_of_field: &'static [(u32, u8)]) -> impl FnMut(u32) -> (u8, u8) {
        move |d| {
            if dummy_of_field.iter().any(|&(dd, f)| dd == d && f == field) {
                (0x20 | (field << 2), 0x8D)
            } else if d > 8 {
                (0x8D, 0xDF)
            } else {
                (0x4B, 0x00)
            }
        }
    }

    const KNOWN: &[(u32, u8)] = &[(8, 2), (10, 3)];

    #[test]
    fn retry_after_a_failed_160_reads_an_lc5_chip_with_the_lc5_dummy() {
        // dummies are still those of 160 (10) but the chip stayed at LC5 (field 2): the misread is MR1 = 0x8D, "field 3"
        let mut chip = fake_chip(2, KNOWN);
        assert_eq!(pick_reg_dummy(KNOWN, 10, &mut chip), 8);
    }

    #[test]
    fn an_lc6_chip_with_120_dummies_is_found_too() {
        // the misread is MR8 = 0x4B, "field 2" = LC5, the field of the dummy in use
        let mut chip = fake_chip(3, KNOWN);
        assert_eq!(pick_reg_dummy(KNOWN, 8, &mut chip), 10);
    }

    #[test]
    fn the_field_alone_would_be_fooled_by_a_neighbouring_register() {
        // the old rule (MR0[4:2] equals the speed's field) accepts both misreads; the vendor and LT checks refuse them
        let field_only = |(mr0, _): (u8, u8), rl: u8| (mr0 >> 2) & 7 == rl;
        assert!(field_only((0x8D, 0xDF), 3)); // LC5 chip read with the 160 dummy
        assert!(field_only((0x4B, 0x00), 2)); // LC6 chip read with the 120 dummy
        let mut lc5 = fake_chip(2, KNOWN);
        assert_eq!(pick_reg_dummy(KNOWN, 10, &mut lc5), 8);
        let mut lc6 = fake_chip(3, KNOWN);
        assert_eq!(pick_reg_dummy(KNOWN, 8, &mut lc6), 10);
    }

    #[test]
    fn a_consistent_current_dummy_is_kept_and_nothing_is_probed_with_one_speed() {
        let mut chip = fake_chip(3, KNOWN);
        assert_eq!(pick_reg_dummy(KNOWN, 10, &mut chip), 10);
        let mut never = |_| -> (u8, u8) { panic!("a single known speed must not probe") };
        assert_eq!(pick_reg_dummy(&[(8, 2)], 8, &mut never), 8);
    }

    #[test]
    fn nothing_consistent_keeps_the_current_dummy() {
        let mut dead = |_| (0xFFu8, 0xFFu8);
        assert_eq!(pick_reg_dummy(KNOWN, 10, &mut dead), 10);
    }

    #[test]
    fn density_decode() {
        let m = ModeRegs { mr2: 0b1100_0111, ..Default::default() };
        assert_eq!((m.density(), m.known_good_die(), m.size_bytes()), (7, 6, 32 << 20));
        assert_eq!(ModeRegs { mr4: 0b0001_1000, ..Default::default() }.refresh(), 3);
    }
}
