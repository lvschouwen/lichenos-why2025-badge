//! Memory the suites share: raw word access, the PSRAM window, the internal-RAM buffers and cache maintenance with its failure count.
use core::ptr::{addr_of_mut, read_volatile, write_volatile};
use core::sync::atomic::{AtomicU32, Ordering::Relaxed};
use lichen_platform_esp32p4::psram::map::{self, CACHED_BASE};

pub const CHIP: usize = 32 << 20;
pub const BUF: usize = 16 << 10;
pub const PSRAM_OFFSETS: Offsets = Offsets {
    read: 0x100_0000,
    write: 0x140_0000,
    copy_dst: 0x180_0000,
    perm: 0x1C0_0000,
    chase: 0x80_0000,
    // the DMA benches run after the chase rings and reuse their 8 MB
    dma: 0x80_0000,
    park: 0xC0_0000,
};
// Every window lies inside the 32 MB chip (an offset past it is not memory: #648's first ring test found that out), clear of the
// others it runs beside, and outside the hole 0x54_0000-0x5C_0000, which only the integrity suite writes.
const _: () = {
    let o = &PSRAM_OFFSETS;
    assert!(o.read + (4 << 20) <= o.write && o.write + (4 << 20) <= o.copy_dst && o.copy_dst + (4 << 20) <= o.perm);
    assert!(o.perm + (1 << 20) <= CHIP);
    assert!(o.chase + (8 << 20) <= o.read && o.chase >= 0x5C_0000);
    assert!(o.dma + (8 << 20) <= o.read);
    // the park test runs after the rings and reuses their window: the CPU region at `park` (1 MB, holds 0xC4AB4C, the #569 flip address) and
    // the DMA region 2 MB above it (1 MB)
    assert!(o.park >= o.chase && o.park + (3 << 20) <= o.chase + (8 << 20));
};
/// Where each test uses the chip (all outside the hole 0x54_0000-0x5C_0000, which only the integrity suite writes).
pub struct Offsets {
    pub read: usize,
    pub write: usize,
    pub copy_dst: usize,
    pub perm: usize,
    pub chase: usize,
    pub dma: usize,
    pub park: usize,
}

/// 128-byte aligned (the larger L2 line) internal-RAM buffers for the CPU tests and the DMA engine.
#[repr(C, align(128))]
pub struct Buf(pub [u8; BUF]);
#[link_section = ".bss.dma_buf.a"]
pub static mut BUF_A: Buf = Buf([0; BUF]);
#[link_section = ".bss.dma_buf.b"]
pub static mut BUF_B: Buf = Buf([0; BUF]);

pub fn buf_a() -> usize {
    addr_of_mut!(BUF_A) as usize
}
pub fn buf_b() -> usize {
    addr_of_mut!(BUF_B) as usize
}

#[inline(always)]
pub fn rd(addr: usize) -> u32 {
    // SAFETY: callers pass a 4-byte aligned address in the mapped PSRAM window or in the probe's own RAM.
    unsafe { read_volatile(addr as *const u32) }
}
#[inline(always)]
pub fn wr(addr: usize, v: u32) {
    // SAFETY: as `rd`.
    unsafe { write_volatile(addr as *mut u32, v) }
}
pub fn cached(off: usize) -> usize {
    CACHED_BASE + off
}

/// Cache maintenance calls that did not return 0 (a sync that never reported DONE): any is a failure of the run.
pub static CACHE_FAILS: AtomicU32 = AtomicU32::new(0);
fn rc(code: i32) {
    if code != 0 {
        CACHE_FAILS.fetch_add(1, Relaxed);
    }
}
/// Write dirty lines back, then drop the range.
pub fn wb_inv(addr: usize, len: usize) {
    // SAFETY: ROM/controller cache maintenance of a range of internal RAM or of the mapped window.
    rc(unsafe { map::cache_writeback_invalidate(addr, len) });
}
/// Drop the range without writing it back.
pub fn inv(addr: usize, len: usize) {
    // SAFETY: as `wb_inv`.
    rc(unsafe { map::cache_invalidate(addr, len) });
}

/// One word of the memory itself (the uncached alias), for diagnosing a bad cached read. Reads only: uncached WRITES are out of scope.
pub fn rd_truth(off: usize) -> u32 {
    rd(CACHED_BASE + 0x4000_0000 + off)
}
