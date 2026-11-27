//! #620 workaround stage (`cpu-workaround`, CPU 360 MHz, PSRAM 120 MHz): PSRAM written only through the cached alias, or by DMA,
//! with the physical range 0x540000-0x5C0000 (map::EXCLUDED_*) kept out. Cases:
//! (a) address-in-address and random data over the whole chip minus the hole, cached writes + write-back, read back uncached
//!     (the memory itself) and cached;
//! (b) the same address pattern written by AXI-DMA M2M from internal RAM, whole chip minus the hole, plus the hole as control
//!     (address and random data);
//! (c) the hole with cached writes (control: does it stay clean through the cached path?).
//! Time budget: the uncached read-back is slow (about 1.3 MB/s), so each full-range case measures its first chunk and samples
//! every k-th 64 KB chunk when the whole range would not fit; the sampling factor is printed.
use core::ptr::{addr_of_mut, read_volatile, write_volatile};
use lichen_platform_esp32p4::psram::dma::{self, Desc};
use lichen_platform_esp32p4::psram::map::{
    self, cached_addr, overlaps_excluded, EXCLUDED_END, EXCLUDED_START, PAGE_BYTES, UNCACHED_OFFSET,
};
use lichen_platform_esp32p4::psram::patterns;

const TOTAL: usize = 32 << 20;
const CHUNK: usize = PAGE_BYTES;
/// Seconds one full-range case may take before it samples.
const CASE_BUDGET_US: u64 = 22_000_000;

#[repr(C, align(64))]
struct Src([u32; CHUNK / 4]);
static mut SRC: Src = Src([0; CHUNK / 4]);
const NSEG: usize = CHUNK / dma::SEG_BYTES;
#[repr(C, align(64))]
struct Dst([u32; 1024]);
static mut DST: Dst = Dst([0; 1024]);
static mut TXD: [Desc; NSEG] = [Desc::zero(); NSEG];
static mut RXD: [Desc; NSEG] = [Desc::zero(); NSEG];

fn now_us() -> u64 {
    const SYSTIMER: usize = 0x500E_2000;
    // SAFETY: SYSTIMER update-and-read (its clock gate was opened by clock::measure_mhz).
    unsafe {
        lichen_platform_esp32p4::mmio::modify32(SYSTIMER + 4, 0, 1 << 30);
        let hi = lichen_platform_esp32p4::mmio::read32(SYSTIMER + 0x40) as u64 & 0xFFFFF;
        let lo = lichen_platform_esp32p4::mmio::read32(SYSTIMER + 0x44) as u64;
        ((hi << 32) | lo) / 16
    }
}

#[derive(Clone, Copy)]
enum Pat {
    Addr,
    Rand(u32),
}
fn word(p: Pat, off: usize) -> u32 {
    match p {
        Pat::Addr => patterns::addr_word(off as u32, false),
        Pat::Rand(seed) => patterns::prng_word(seed, (off / 4) as u32),
    }
}

struct Tally {
    words: u64,
    bad: u32,
    bit8: u32,
    other: u32,
    in_class: u32,
    first: [u32; 4],
}
impl Tally {
    const fn new() -> Self {
        Tally { words: 0, bad: 0, bit8: 0, other: 0, in_class: 0, first: [0; 4] }
    }
    fn add(&mut self, off: usize, exp: u32, got: u32) {
        self.words += 1;
        if exp != got {
            if (self.bad as usize) < 4 {
                self.first[self.bad as usize] = off as u32;
            }
            self.bad += 1;
            if exp ^ got == 0x100 {
                self.bit8 += 1;
            } else {
                self.other += 1;
            }
            if off & 0x7FF == 0x400 {
                self.in_class += 1;
            }
        }
    }
    fn show(&self, name: core::fmt::Arguments, note: core::fmt::Arguments) {
        pl!(
            "WORKAROUND {name}: {} bad of {} words (bit 8 only {}, other {}, at 0x400 mod 0x800: {}) first {:#x} {:#x} {:#x} {:#x}; {note}",
            self.bad,
            self.words,
            self.bit8,
            self.other,
            self.in_class,
            self.first[0],
            self.first[1],
            self.first[2],
            self.first[3]
        );
        lichen_platform_esp32p4::uart::drain();
    }
}

fn rd(addr: usize) -> u32 {
    // SAFETY: inside the mapped 32 MB window, aligned.
    unsafe { read_volatile(addr as *const u32) }
}

/// Cached write of one chunk, write-back, read back uncached (truth) and cached.
fn cached_chunk(p: Pat, off: usize, truth: &mut Tally, cached: &mut Tally) {
    let mut a = 0;
    while a < CHUNK {
        // SAFETY: mapped window, cached alias.
        unsafe { write_volatile(cached_addr(off + a) as *mut u32, word(p, off + a)) };
        a += 4;
    }
    // SAFETY: ROM cache maintenance of the chunk.
    unsafe { map::cache_writeback_invalidate(cached_addr(off), CHUNK) };
    let mut a = 0;
    while a < CHUNK {
        truth.add(off + a, word(p, off + a), rd(cached_addr(off + a) + UNCACHED_OFFSET));
        a += 4;
    }
    let mut a = 0;
    while a < CHUNK {
        cached.add(off + a, word(p, off + a), rd(cached_addr(off + a)));
        a += 4;
    }
    // SAFETY: as above.
    unsafe { map::cache_invalidate(cached_addr(off), CHUNK) };
}

/// Chunks of the usable range (or the hole) with a sampling stride from the measured time of the first chunk.
fn chunks(hole: bool) -> impl Iterator<Item = usize> {
    (0..TOTAL / CHUNK).map(|i| i * CHUNK).filter(move |&o| overlaps_excluded(o, CHUNK) == hole)
}

fn full_cached(name: &str, p: Pat, hole: bool) {
    let (mut truth, mut cached) = (Tally::new(), Tally::new());
    let n = chunks(hole).count() as u64;
    let t0 = now_us();
    let mut it = chunks(hole);
    let Some(first) = it.next() else { return };
    cached_chunk(p, first, &mut truth, &mut cached);
    let per = (now_us() - t0).max(1);
    let k = ((n * per).div_ceil(CASE_BUDGET_US)).max(1) as usize;
    let mut done = 1;
    for (i, off) in it.enumerate() {
        if (i + 1) % k == 0 {
            cached_chunk(p, off, &mut truth, &mut cached);
            done += 1;
        }
    }
    let secs = (now_us() - t0) / 1_000_000;
    truth.show(
        format_args!("{name}, read back uncached (the memory itself)"),
        format_args!("{done} of {n} chunks (every {k}th), {secs} s"),
    );
    cached.show(format_args!("{name}, read back cached"), format_args!("same chunks"));
}

/// One chunk by DMA: source in internal RAM, destination the cached alias; returns the DMA result and the number of words that
/// differed when read uncached right after the DMA, before any cache maintenance (`imm`, only if asked).
fn dma_chunk(
    p: Pat,
    off: usize,
    imm: Option<&mut Tally>,
    truth: &mut Tally,
    cached: &mut Tally,
) -> Result<(), dma::DmaError> {
    let src = addr_of_mut!(SRC) as usize;
    let mut a = 0;
    while a < CHUNK {
        // SAFETY: SRC is a private static, written only here.
        unsafe { write_volatile((src + a) as *mut u32, word(p, off + a)) };
        a += 4;
    }
    // SAFETY: statics used only by this stage; ROM cache maintenance of internal and PSRAM ranges, DMA over valid memory.
    let r = unsafe {
        map::cache_writeback_invalidate(src, CHUNK);
        map::cache_invalidate(cached_addr(off), CHUNK);
        let tx = addr_of_mut!(TXD) as *mut Desc;
        let rx = addr_of_mut!(RXD) as *mut Desc;
        let txs = core::slice::from_raw_parts_mut(tx, NSEG);
        let rxs = core::slice::from_raw_parts_mut(rx, NSEG);
        dma::build_chain(txs, src as u32, CHUNK, true);
        dma::build_chain(rxs, cached_addr(off) as u32, CHUNK, false);
        map::cache_writeback_invalidate(tx as usize, NSEG * 16);
        map::cache_writeback_invalidate(rx as usize, NSEG * 16);
        dma::run(&*tx, &*rx, 5_000_000)
    };
    r?;
    if let Some(t) = imm {
        let mut a = 0;
        while a < CHUNK {
            t.add(off + a, word(p, off + a), rd(cached_addr(off + a) + UNCACHED_OFFSET));
            a += 4;
        }
    }
    // SAFETY: ROM cache maintenance (writes back lines the DMA left dirty in L2, then drops them).
    unsafe { map::cache_writeback_invalidate(cached_addr(off), CHUNK) };
    let mut a = 0;
    while a < CHUNK {
        truth.add(off + a, word(p, off + a), rd(cached_addr(off + a) + UNCACHED_OFFSET));
        a += 4;
    }
    let mut a = 0;
    while a < CHUNK {
        cached.add(off + a, word(p, off + a), rd(cached_addr(off + a)));
        a += 4;
    }
    // SAFETY: as above.
    unsafe { map::cache_invalidate(cached_addr(off), CHUNK) };
    Ok(())
}

fn full_dma(name: &str, p: Pat, hole: bool) {
    let (mut imm, mut truth, mut cached) = (Tally::new(), Tally::new(), Tally::new());
    let n = chunks(hole).count() as u64;
    let t0 = now_us();
    let mut it = chunks(hole);
    let Some(first) = it.next() else { return };
    if let Err(e) = dma_chunk(p, first, Some(&mut imm), &mut truth, &mut cached) {
        pl!("WORKAROUND {name}: DMA failed on the first chunk: {e:?}");
        return;
    }
    let per = (now_us() - t0).max(1);
    let k = ((n * per).div_ceil(CASE_BUDGET_US)).max(1) as usize;
    let mut done = 1;
    for (i, off) in it.enumerate() {
        if (i + 1) % k == 0 {
            let want_imm = done < 16;
            if let Err(e) = dma_chunk(p, off, if want_imm { Some(&mut imm) } else { None }, &mut truth, &mut cached) {
                pl!("WORKAROUND {name}: DMA failed at {off:#x}: {e:?}");
                break;
            }
            done += 1;
        }
    }
    let secs = (now_us() - t0) / 1_000_000;
    imm.show(
        format_args!("{name}, uncached read right after the DMA, no cache operation (first 16 chunks)"),
        format_args!("tells whether DMA data is already in PSRAM"),
    );
    truth.show(
        format_args!("{name}, uncached read after write-back (the memory itself)"),
        format_args!("{done} of {n} chunks (every {k}th), {secs} s"),
    );
    cached.show(format_args!("{name}, cached read"), format_args!("same chunks"));
}

/// DMA engine check without PSRAM: 4 KB from SRC to an internal buffer, with the cache return codes and, on failure, the
/// non-zero words of the AXI-DMA register block.
fn dma_selftest() {
    let src = addr_of_mut!(SRC) as usize;
    let dst = addr_of_mut!(DST) as usize;
    for i in 0..1024usize {
        // SAFETY: private statics.
        unsafe {
            write_volatile((src + 4 * i) as *mut u32, 0xC0DE_0000 + i as u32);
            write_volatile((dst + 4 * i) as *mut u32, 0);
        }
    }
    // SAFETY: statics used only by this stage; ROM cache maintenance and DMA over valid internal memory.
    let (rcs, rcd, rct, rcr, r) = unsafe {
        let tx = addr_of_mut!(TXD) as *mut Desc;
        let rx = addr_of_mut!(RXD) as *mut Desc;
        dma::build_chain(core::slice::from_raw_parts_mut(tx, NSEG), src as u32, 4096, true);
        dma::build_chain(core::slice::from_raw_parts_mut(rx, NSEG), dst as u32, 4096, false);
        let rcs = map::cache_writeback_invalidate(src, 4096);
        let rcd = map::cache_writeback_invalidate(dst, 4096);
        let rct = map::cache_writeback_invalidate(tx as usize, 2 * 16);
        let rcr = map::cache_writeback_invalidate(rx as usize, 2 * 16);
        (rcs, rcd, rct, rcr, dma::run(&*tx, &*rx, 5_000_000))
    };
    // SAFETY: cache maintenance of the destination before reading it.
    let rci = unsafe { map::cache_invalidate(dst, 4096) };
    let mut bad = 0;
    for i in 0..1024usize {
        // SAFETY: private static.
        if unsafe { read_volatile((dst + 4 * i) as *const u32) } != 0xC0DE_0000 + i as u32 {
            bad += 1;
        }
    }
    pl!("WORKAROUND dma self-test internal -> internal 4 KB: {r:?}, {bad} of 1024 words wrong; cache rc src {rcs} dst {rcd} txdesc {rct} rxdesc {rcr} invalidate {rci}");
    if r.is_err() || bad != 0 {
        for off in (0..0x2D0usize).step_by(4) {
            let v = rd(0x5008_A000 + off);
            if v != 0 {
                pl!("WORKAROUND   axi_dma+{off:#05x} = {v:#010x}");
            }
        }
    }
}

pub fn run(cpu_mhz: u32) {
    // SAFETY: ROM cache maintenance, MMU window, DMA block; this stage owns all of them.
    unsafe {
        map::cache_invalidate_all_l2();
        map::map_identity(TOTAL / PAGE_BYTES);
        dma::init();
    }
    pl!("WORKAROUND stage: excluded physical range {EXCLUDED_START:#x}-{EXCLUDED_END:#x}, cpu {cpu_mhz} MHz");
    dma_selftest();
    let t0 = now_us();
    full_cached("(a) address-in-address, cached write + write-back, whole chip minus the hole", Pat::Addr, false);
    full_cached(
        "(a) random 0x12345678, cached write + write-back, whole chip minus the hole",
        Pat::Rand(0x1234_5678),
        false,
    );
    full_cached("(c) hole, address-in-address, cached write + write-back", Pat::Addr, true);
    full_cached("(c) hole, random 0xA5A55A5A, cached write + write-back", Pat::Rand(0xA5A5_5A5A), true);
    full_dma("(b) hole control, address-in-address, DMA", Pat::Addr, true);
    full_dma("(b) hole control, random 0x0BADF00D, DMA", Pat::Rand(0x0BAD_F00D), true);
    full_dma("(b) address-in-address, DMA, whole chip minus the hole", Pat::Addr, false);
    pl!("WORKAROUND elapsed {} s", (now_us() - t0) / 1_000_000);
}
