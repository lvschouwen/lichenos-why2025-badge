//! #620 workaround stage (`cpu-workaround`, CPU 360 MHz, PSRAM 120 MHz): PSRAM written only through the cached alias, or by DMA,
//! with the physical range 0x540000-0x5C0000 (map::EXCLUDED_*) kept out. Cases:
//! (a) address-in-address and random data over the whole chip minus the hole, cached writes + write-back, read back uncached
//!     (the memory itself) and cached;
//! (b) the same address pattern written by AXI-DMA M2M from internal RAM, whole chip minus the hole, plus the hole as control
//!     (address and random data);
//! (c) the hole with cached writes (control: does it stay clean through the cached path?).
//! Time budget: the uncached read-back is slow (about 1.3 MB/s), so each full-range case measures its first chunk and samples
//! every k-th 64 KB chunk when the whole range would not fit; the sampling factor is printed.
use crate::tests::UNCACHED_OFFSET;
use core::ptr::{addr_of_mut, read_volatile, write_volatile};
use core::sync::atomic::{AtomicU32, Ordering::Relaxed};
use lichen_platform_esp32p4::psram::dma::{self, Desc};
use lichen_platform_esp32p4::psram::map::{
    self, cached_addr, overlaps_excluded, CACHED_BASE, EXCLUDED_END, EXCLUDED_START, PAGE_BYTES,
};
use lichen_platform_esp32p4::psram::{patterns, systimer};

const TOTAL: usize = 32 << 20;
const CHUNK: usize = PAGE_BYTES;
/// Seconds one full-range case may take before it samples.
const CASE_BUDGET_US: u64 = 22_000_000;

#[repr(C, align(128))]
struct Src([u32; CHUNK / 4]);
static mut SRC: Src = Src([0; CHUNK / 4]);
const NSEG: usize = CHUNK / dma::SEG_BYTES;
#[repr(C, align(128))]
struct Dst([u32; 1024]);
static mut DST: Dst = Dst([0; 1024]);
static mut TXD: [Desc; NSEG] = [Desc::zero(); NSEG];
static mut RXD: [Desc; NSEG] = [Desc::zero(); NSEG];

/// Verdict inputs of `CHECK 620.workaround`: bad words, DMA errors, cache maintenance calls that returned non-zero.
static BAD: AtomicU32 = AtomicU32::new(0);
static DMA_ERR: AtomicU32 = AtomicU32::new(0);
static CACHE_FAIL: AtomicU32 = AtomicU32::new(0);

fn now_us() -> u64 {
    systimer::now_us().unwrap_or(0)
}

fn rc(code: i32) {
    if code != 0 {
        CACHE_FAIL.fetch_add(1, Relaxed);
    }
}
fn wb_inv(addr: usize, len: usize) {
    // SAFETY: cache maintenance of a range of internal RAM or of the mapped PSRAM window.
    rc(unsafe { map::cache_writeback_invalidate(addr, len) });
}
fn inv(addr: usize, len: usize) {
    // SAFETY: as above.
    rc(unsafe { map::cache_invalidate(addr, len) });
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

/// Cached-alias address of an offset: the platform API for the usable range; the hole (a diagnostic control, its pages mapped
/// by `map_identity_with_hole`) is addressed by the raw window base because `cached_addr` refuses it by design.
fn caddr(off: usize, hole: bool) -> usize {
    if hole {
        assert!(overlaps_excluded(off, 4));
        CACHED_BASE + off
    } else {
        cached_addr(off).expect("a usable offset")
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
        BAD.fetch_add(self.bad, Relaxed);
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
/// The memory itself: an uncached read (diagnostics).
fn rd_truth(off: usize, hole: bool) -> u32 {
    rd(caddr(off, hole) + UNCACHED_OFFSET)
}

/// Cached write of one chunk, write-back, read back uncached (truth) and cached.
fn cached_chunk(p: Pat, off: usize, hole: bool, truth: &mut Tally, cached: &mut Tally) {
    let base = caddr(off, hole);
    let mut a = 0;
    while a < CHUNK {
        // SAFETY: mapped window, cached alias.
        unsafe { write_volatile((base + a) as *mut u32, word(p, off + a)) };
        a += 4;
    }
    wb_inv(base, CHUNK);
    let mut a = 0;
    while a < CHUNK {
        truth.add(off + a, word(p, off + a), rd_truth(off + a, hole));
        a += 4;
    }
    let mut a = 0;
    while a < CHUNK {
        cached.add(off + a, word(p, off + a), rd(base + a));
        a += 4;
    }
    inv(base, CHUNK);
}

/// Chunks of the usable range (or the hole).
fn chunks(hole: bool) -> impl Iterator<Item = usize> {
    (0..TOTAL / CHUNK).map(|i| i * CHUNK).filter(move |&o| overlaps_excluded(o, CHUNK) == hole)
}

fn full_cached(name: &str, p: Pat, hole: bool) {
    let (mut truth, mut cached) = (Tally::new(), Tally::new());
    let n = chunks(hole).count() as u64;
    let t0 = now_us();
    let mut it = chunks(hole);
    let Some(first) = it.next() else { return };
    cached_chunk(p, first, hole, &mut truth, &mut cached);
    let per = now_us().saturating_sub(t0).max(1);
    let k = ((n * per).div_ceil(CASE_BUDGET_US)).max(1) as usize;
    let mut done = 1;
    for (i, off) in it.enumerate() {
        if (i + 1) % k == 0 {
            cached_chunk(p, off, hole, &mut truth, &mut cached);
            done += 1;
        }
    }
    let secs = now_us().saturating_sub(t0) / 1_000_000;
    truth.show(
        format_args!("{name}, read back uncached (the memory itself)"),
        format_args!("{done} of {n} chunks (every {k}th), {secs} s"),
    );
    cached.show(format_args!("{name}, read back cached"), format_args!("same chunks"));
}

/// One chunk by DMA. Cache contract (psram/dma.rs): before the DMA write back and invalidate source and destination, after it
/// invalidate the destination only. `imm` is read uncached right after the DMA, before any cache operation.
fn dma_chunk(
    p: Pat,
    off: usize,
    hole: bool,
    imm: Option<&mut Tally>,
    truth: &mut Tally,
    cached: &mut Tally,
) -> Result<(), dma::DmaError> {
    let src = addr_of_mut!(SRC) as usize;
    let dst = caddr(off, hole);
    let mut a = 0;
    while a < CHUNK {
        // SAFETY: SRC is a private static, written only here.
        unsafe { write_volatile((src + a) as *mut u32, word(p, off + a)) };
        a += 4;
    }
    wb_inv(src, CHUNK);
    wb_inv(dst, CHUNK);
    // SAFETY: private statics used only by this stage; descriptors in internal RAM (.bss), buffers 128-byte aligned; the DMA
    // result is checked.
    let r = unsafe {
        let tx = addr_of_mut!(TXD) as *mut Desc;
        let rx = addr_of_mut!(RXD) as *mut Desc;
        dma::build_chain(core::slice::from_raw_parts_mut(tx, NSEG), src as u32, CHUNK, true);
        dma::build_chain(core::slice::from_raw_parts_mut(rx, NSEG), dst as u32, CHUNK, false);
        wb_inv(tx as usize, NSEG * 16);
        wb_inv(rx as usize, NSEG * 16);
        dma::run(tx, rx, 5_000_000)
    };
    if let Err(e) = r {
        DMA_ERR.fetch_add(1, Relaxed);
        return Err(e);
    }
    if let Some(t) = imm {
        let mut a = 0;
        while a < CHUNK {
            t.add(off + a, word(p, off + a), rd_truth(off + a, hole));
            a += 4;
        }
    }
    inv(dst, CHUNK);
    let mut a = 0;
    while a < CHUNK {
        truth.add(off + a, word(p, off + a), rd_truth(off + a, hole));
        a += 4;
    }
    let mut a = 0;
    while a < CHUNK {
        cached.add(off + a, word(p, off + a), rd(dst + a));
        a += 4;
    }
    inv(dst, CHUNK);
    Ok(())
}

fn full_dma(name: &str, p: Pat, hole: bool) {
    let (mut imm, mut truth, mut cached) = (Tally::new(), Tally::new(), Tally::new());
    let n = chunks(hole).count() as u64;
    let t0 = now_us();
    let mut it = chunks(hole);
    let Some(first) = it.next() else { return };
    if let Err(e) = dma_chunk(p, first, hole, Some(&mut imm), &mut truth, &mut cached) {
        pl!("WORKAROUND {name}: DMA failed on the first chunk: {e:?}");
        return;
    }
    let per = now_us().saturating_sub(t0).max(1);
    let k = ((n * per).div_ceil(CASE_BUDGET_US)).max(1) as usize;
    let mut done = 1;
    for (i, off) in it.enumerate() {
        if (i + 1) % k == 0 {
            let want_imm = done < 16;
            if let Err(e) =
                dma_chunk(p, off, hole, if want_imm { Some(&mut imm) } else { None }, &mut truth, &mut cached)
            {
                pl!("WORKAROUND {name}: DMA failed at {off:#x}: {e:?}");
                break;
            }
            done += 1;
        }
    }
    let secs = now_us().saturating_sub(t0) / 1_000_000;
    imm.show(
        format_args!("{name}, uncached read right after the DMA, no cache operation (first 16 chunks)"),
        format_args!("tells whether DMA data is already in PSRAM"),
    );
    truth.show(
        format_args!("{name}, uncached read after the destination was invalidated (the memory itself)"),
        format_args!("{done} of {n} chunks (every {k}th), {secs} s"),
    );
    cached.show(format_args!("{name}, cached read"), format_args!("same chunks"));
}

/// DMA engine check without PSRAM: 4 KB from SRC to an internal buffer, then the non-zero AXI-DMA registers on failure.
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
    wb_inv(src, 4096);
    wb_inv(dst, 4096);
    // SAFETY: private statics, descriptors in internal RAM, buffers aligned.
    let r = unsafe {
        let tx = addr_of_mut!(TXD) as *mut Desc;
        let rx = addr_of_mut!(RXD) as *mut Desc;
        dma::build_chain(core::slice::from_raw_parts_mut(tx, NSEG), src as u32, 4096, true);
        dma::build_chain(core::slice::from_raw_parts_mut(rx, NSEG), dst as u32, 4096, false);
        wb_inv(tx as usize, 2 * 16);
        wb_inv(rx as usize, 2 * 16);
        dma::run(tx, rx, 5_000_000)
    };
    inv(dst, 4096);
    let mut bad = 0;
    for i in 0..1024usize {
        // SAFETY: private static.
        if unsafe { read_volatile((dst + 4 * i) as *const u32) } != 0xC0DE_0000 + i as u32 {
            bad += 1;
        }
    }
    if r.is_err() || bad != 0 {
        DMA_ERR.fetch_add(1, Relaxed);
    }
    pl!("WORKAROUND dma self-test internal -> internal 4 KB: {r:?}, {bad} of 1024 words wrong");
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
    // SAFETY: ROM cache maintenance, MMU window, DMA block; this stage owns all of them. The hole pages stay unmapped until the
    // control cases need them.
    unsafe {
        map::cache_invalidate_all_l2();
        map::map_identity(TOTAL / PAGE_BYTES);
        dma::init();
    }
    pl!(
        "WORKAROUND stage: excluded physical range {EXCLUDED_START:#x}-{EXCLUDED_END:#x} (unmapped), cpu {cpu_mhz} MHz"
    );
    dma_selftest();
    let t0 = now_us();
    full_cached("(a) address-in-address, cached write + write-back, whole chip minus the hole", Pat::Addr, false);
    full_cached(
        "(a) random 0x12345678, cached write + write-back, whole chip minus the hole",
        Pat::Rand(0x1234_5678),
        false,
    );
    full_dma("(b) address-in-address, DMA, whole chip minus the hole", Pat::Addr, false);
    // the controls: map the hole for them only
    // SAFETY: MMU window; diagnostics only.
    unsafe { map::map_identity_with_hole(TOTAL / PAGE_BYTES) };
    full_cached("(c) hole, address-in-address, cached write + write-back", Pat::Addr, true);
    full_cached("(c) hole, random 0xA5A55A5A, cached write + write-back", Pat::Rand(0xA5A5_5A5A), true);
    full_dma("(b) hole control, address-in-address, DMA", Pat::Addr, true);
    full_dma("(b) hole control, random 0x0BADF00D, DMA", Pat::Rand(0x0BAD_F00D), true);
    pl!("WORKAROUND elapsed {} s", now_us().saturating_sub(t0) / 1_000_000);
    let (bad, derr, cfail) = (BAD.load(Relaxed), DMA_ERR.load(Relaxed), CACHE_FAIL.load(Relaxed));
    let ok = bad == 0 && derr == 0 && cfail == 0;
    pl!(
        "CHECK 620.workaround {} bad words {bad}, DMA errors {derr}, cache calls that failed {cfail}",
        if ok { "PASS" } else { "FAIL" }
    );
}
