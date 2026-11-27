//! AXI-DMA memory-to-memory copy into PSRAM (#620 workaround test): channel 0, TX reads the source, RX writes the destination.
//! Register words recorded from ESP-IDF v6.1 `axi_dma_ll_*` / `gdma_ll_*` (`esp_hal_dma/esp32p4/include/hal/`) the way psram/regs.rs
//! was. The destination is a cached-alias PSRAM address (the AXI-DMA external range is 0x4000_0000-0x4C00_0000); the caller owns
//! cache maintenance (write back the source, invalidate the destination before and after).
use crate::mmio::{modify32, read32, write32};

const AXI_DMA: usize = 0x5008_A000;
const HP_CLKRST: usize = 0x500E_6000;
const SOC_CLK_CTRL1: usize = HP_CLKRST + 0x18; // reg_axi_pdma_sys_clk_en bit 4
const HP_RST_EN1: usize = HP_CLKRST + 0xC4; // reg_rst_en_axi_pdma bit 2
const MISC_CONF: usize = AXI_DMA + 0x2A8; // clk_en bit 4
const MEM_RANGE: [(usize, u32); 4] =
    [(0x27C, 0x4FC0_0000), (0x280, 0x4FFC_0000), (0x284, 0x4000_0000), (0x288, 0x4C00_0000)];
const RX_INT_RAW: usize = AXI_DMA; // raw (0x004 is the masked status)
const RX_INT_CLR: usize = AXI_DMA + 0x00C;
const RX_CONF0: usize = AXI_DMA + 0x010; // in_rst bit 0, mem_trans_en bit 2, burst size [6:4]
const RX_CONF1: usize = AXI_DMA + 0x014; // owner check bit 12
const RX_LINK: usize = AXI_DMA + 0x020; // inlink_start bit 2
const RX_DESC: usize = AXI_DMA + 0x024;
const RX_PERI_SEL: usize = AXI_DMA + 0x044; // [5:0]
const TX_INT_RAW: usize = AXI_DMA + 0x138;
const TX_INT_CLR: usize = AXI_DMA + 0x144;
const TX_CONF0: usize = AXI_DMA + 0x148; // out_rst bit 0, auto write back bit 2, eof mode bit 3, burst size [7:5]
const TX_CONF1: usize = AXI_DMA + 0x14C; // owner check bit 12
const TX_LINK: usize = AXI_DMA + 0x158; // outlink_start bit 1
const TX_DESC: usize = AXI_DMA + 0x15C;
const TX_PERI_SEL: usize = AXI_DMA + 0x17C;
/// First free M2M "peripheral" id (AXI_DMA_LL_M2M_FREE_PERIPH_ID_MASK = 0xFFC0).
const M2M_ID: u32 = 6;
pub const SEG_BYTES: usize = 2048;

/// One AXI-DMA descriptor (`dma_descriptor_align8_t`, 16 bytes, 8-byte aligned).
#[repr(C, align(8))]
#[derive(Clone, Copy)]
pub struct Desc {
    pub dw0: u32,
    pub buffer: u32,
    pub next: u32,
    _pad: u32,
}
impl Desc {
    pub const fn zero() -> Desc {
        Desc { dw0: 0, buffer: 0, next: 0, _pad: 0 }
    }
}

/// Descriptor word 0: size [11:0], length [23:12], suc_eof bit 30, owner bit 31 (1 = DMA).
pub const fn dw0(size: u32, length: u32, eof: bool) -> u32 {
    (size & 0xFFF) | ((length & 0xFFF) << 12) | ((eof as u32) << 30) | (1 << 31)
}

/// Build a chain for `len` bytes at `buf` in `SEG_BYTES` pieces. TX descriptors carry the data length, RX ones only the size.
/// Returns the number of descriptors used (`descs[..n]`); the last one has `suc_eof` and no next.
pub fn build_chain(descs: &mut [Desc], buf: u32, len: usize, tx: bool) -> usize {
    let n = len.div_ceil(SEG_BYTES);
    assert!(n <= descs.len());
    for i in 0..n {
        let seg = (len - i * SEG_BYTES).min(SEG_BYTES) as u32;
        let last = i + 1 == n;
        let next = if last { 0 } else { (&descs[i + 1] as *const Desc) as u32 };
        descs[i] =
            Desc { dw0: dw0(seg, if tx { seg } else { 0 }, last), buffer: buf + (i * SEG_BYTES) as u32, next, _pad: 0 };
    }
    n
}

#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum DmaError {
    /// No end of frame in time: the RX and TX raw interrupt words at that point.
    Timeout(u32, u32),
    /// RX interrupt raw word with an error bit (descriptor error, error EOF).
    Fault(u32),
}

/// Clock, reset, memory ranges, channel 0 in M2M mode (IDF `gdma_new_axi_channel` + `gdma_connect(M2M)`).
///
/// # Safety
/// Owns the AXI-DMA block and channel 0; one caller.
pub unsafe fn init() {
    modify32(SOC_CLK_CTRL1, 0, 1 << 4);
    modify32(HP_RST_EN1, 0, 1 << 2);
    modify32(HP_RST_EN1, 1 << 2, 0);
    modify32(MISC_CONF, 0, 1 << 4);
    for (off, v) in MEM_RANGE {
        write32(AXI_DMA + off, v);
    }
    // channel resets, M2M connect, burst size 32 B, owner check off, no write-back, eof mode 0
    modify32(RX_CONF0, 0, 1);
    modify32(RX_CONF0, 1, 0);
    modify32(TX_CONF0, 0, 1);
    modify32(TX_CONF0, 1, 0);
    modify32(RX_CONF0, 0x4, 0x4);
    modify32(RX_PERI_SEL, 0x3F, M2M_ID);
    modify32(TX_PERI_SEL, 0x3F, M2M_ID);
    modify32(RX_CONF0, 0x70, 0x20);
    modify32(TX_CONF0, 0xE0, 0x40);
    modify32(RX_CONF1, 1 << 12, 0);
    modify32(TX_CONF1, 1 << 12, 0);
    modify32(TX_CONF0, 0xC, 0);
}

/// Run one prepared pair of chains (`tx` reads, `rx` writes) and wait for the RX end of frame.
///
/// # Safety
/// Descriptor and buffer memory must stay valid and cache-clean for the DMA; call after `init`.
pub unsafe fn run(tx: &Desc, rx: &Desc, spins: u32) -> Result<(), DmaError> {
    write32(RX_INT_CLR, 0xFFFF_FFFF);
    write32(TX_INT_CLR, 0xFFFF_FFFF);
    write32(RX_DESC, rx as *const Desc as u32);
    write32(TX_DESC, tx as *const Desc as u32);
    modify32(RX_LINK, 0x4, 0x4);
    modify32(TX_LINK, 0x2, 0x2);
    let mut n = 0;
    loop {
        let raw = read32(RX_INT_RAW);
        if raw & 0x2 != 0 {
            return Ok(()); // IN_SUC_EOF
        }
        if raw & 0xC != 0 {
            return Err(DmaError::Fault(raw)); // IN_ERR_EOF, IN_DSCR_ERR
        }
        n += 1;
        if n > spins {
            return Err(DmaError::Timeout(raw, read32(TX_INT_RAW)));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn descriptor_words() {
        assert_eq!(dw0(2048, 2048, false), 0x8000_0000 | 0x800 | (0x800 << 12));
        assert_eq!(dw0(4095, 4095, true) >> 30, 3);
        assert_eq!(dw0(16, 0, false) & 0xFFFF_F000, 0x8000_0000);
    }

    #[test]
    fn chain_covers_the_length_in_segments() {
        let mut d = [Desc::zero(); 40];
        let n = build_chain(&mut d, 0x4800_0000, 64 * 1024, true);
        assert_eq!(n, 32);
        assert_eq!(d[0].buffer, 0x4800_0000);
        assert_eq!(d[31].buffer, 0x4800_0000 + 31 * 2048);
        assert_eq!(d[31].next, 0);
        assert!(d[31].dw0 >> 30 & 1 == 1 && d[30].dw0 >> 30 & 1 == 0);
        assert_eq!((d[0].dw0 >> 12) & 0xFFF, 2048);
        let n = build_chain(&mut d, 0x4800_0000, 3000, false);
        assert_eq!(n, 2);
        assert_eq!(d[1].dw0 & 0xFFF, 952);
        assert_eq!((d[1].dw0 >> 12) & 0xFFF, 0); // RX: length filled by the DMA
    }
}
