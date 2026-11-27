//! Pattern tests of the 32 MB PSRAM at 120 MHz, through the cached and the uncached alias.
//!
//! Layout: `map_identity` puts physical page n at virtual page n. The uncached alias is the memory truth (every access
//! is a PSRAM transaction); the cached alias goes through the L1/L2 caches, so a cached test writes, writes the lines
//! back, and reads the truth through the uncached alias. Failures print the first few addresses and a per-bit count of
//! flipped bits (the #86 / #569 history: bits 8 and 9, 0 -> 1).
use core::ptr::{read_volatile, write_volatile};
use lichen_platform_esp32p4::psram::map::{self, CACHED_BASE, PAGE_BYTES};

/// The uncached alias offset: the platform keeps it private, the diagnostics here (single-word uncached stores, #620) need it.
pub const UNCACHED_OFFSET: usize = 0x4000_0000;
use lichen_platform_esp32p4::psram::{mspi::delay_us, patterns};

pub const PSRAM_BYTES: usize = 32 << 20;
/// Bytes the sweeping tests cover (the whole chip took minutes; a bench run is at most 120 s).
const TEST_BYTES: usize = 6 << 20;
const TEST_OFF: usize = 0x50_0000; // includes the 0x55xxxx-0x58xxxx addresses that failed at CPU 40 MHz
const UNC: usize = CACHED_BASE + UNCACHED_OFFSET;
const CHUNK: usize = PAGE_BYTES; // 64 KB: fits the caches, so a chunk is written back and verified in one go

struct Tally {
    words: u64,
    bad: u64,
    bit_flips: [u32; 32],
    first: [(usize, u32, u32); 16],
}
impl Tally {
    fn new() -> Self {
        Tally { words: 0, bad: 0, bit_flips: [0; 32], first: [(0, 0, 0); 16] }
    }
    fn check(&mut self, addr: usize, expected: u32, got: u32) {
        self.words += 1;
        if expected != got {
            if (self.bad as usize) < self.first.len() {
                self.first[self.bad as usize] = (addr, expected, got);
            }
            self.bad += 1;
            let d = expected ^ got;
            for (b, n) in self.bit_flips.iter_mut().enumerate() {
                *n += (d >> b) & 1;
            }
        }
    }
}

fn verdict(name: core::fmt::Arguments, t: &Tally) -> bool {
    let ok = t.bad == 0;
    pl!("psram: TEST {name} {} ({} words, {} bad)", if ok { "PASS" } else { "FAIL" }, t.words, t.bad);
    if !ok {
        for &(a, e, g) in t.first.iter().take(t.bad.min(16) as usize) {
            pl!(
                "psram:   at {a:#010x}: expected {e:#010x} got {g:#010x} (flipped bits {})",
                patterns::flipped_bits(e, g)
            );
        }
        for (b, &n) in t.bit_flips.iter().enumerate() {
            if n != 0 {
                pl!("psram:   bit {b}: {n} words");
            }
        }
    }
    ok
}

fn w(addr: usize, v: u32) {
    // SAFETY: addr is inside the 32 MB window `map_identity` mapped, 4-byte aligned.
    unsafe { write_volatile(addr as *mut u32, v) }
}
fn r(addr: usize) -> u32 {
    // SAFETY: as `w`.
    unsafe { read_volatile(addr as *const u32) }
}

/// Words at the start, end and page seams of the window, with every walking-ones and walking-zeros pattern.
fn walking(base: usize, name: &str) -> bool {
    let mut t = Tally::new();
    let spots = [
        0usize,
        4,
        0x8000,
        PAGE_BYTES - 4,
        PAGE_BYTES,
        0x10_0000,
        0x40_0000 - 4,
        0x80_0000,
        0x100_0000,
        PSRAM_BYTES - 8,
        PSRAM_BYTES - 4,
    ];
    for &off in &spots {
        let a = base + off;
        for n in 0..32 {
            for v in [patterns::walking_one(n), patterns::walking_zero(n)] {
                w(a, v);
                t.check(a, v, r(a));
            }
        }
    }
    verdict(format_args!("{name}"), &t)
}

/// Address-in-address over the whole window through the uncached alias, both polarities.
fn addr_in_addr_uncached() -> bool {
    let mut ok = true;
    for inverted in [false, true] {
        let mut t = Tally::new();
        let mut a = TEST_OFF;
        while a < TEST_OFF + TEST_BYTES {
            w(UNC + a, patterns::addr_word(a as u32, inverted));
            a += 4;
        }
        let mut a = TEST_OFF;
        while a < TEST_OFF + TEST_BYTES {
            t.check(UNC + a, patterns::addr_word(a as u32, inverted), r(UNC + a));
            a += 4;
        }
        ok &= verdict(
            format_args!(
                "address-in-address, uncached, {}, {} MB at {TEST_OFF:#x}",
                if inverted { "inverted" } else { "plain" },
                TEST_BYTES >> 20
            ),
            &t,
        );
        PERF_BYTES.fetch_add(2 * TEST_BYTES as u32, core::sync::atomic::Ordering::Relaxed);
        PERF_ERRORS.fetch_add(t.bad as u32, core::sync::atomic::Ordering::Relaxed);
    }
    ok
}

/// A pseudo-random fill of the whole window through the cached alias, a chunk at a time: write cached, write the lines
/// back, read the truth through the uncached alias; then drop the lines and read the same chunk cached again.
fn random_cached(seed: u32) -> bool {
    let (mut truth, mut again) = (Tally::new(), Tally::new());
    let mut off = TEST_OFF;
    while off < TEST_OFF + TEST_BYTES {
        let mut a = 0;
        while a < CHUNK {
            let i = ((off + a) / 4) as u32;
            w(CACHED_BASE + off + a, patterns::prng_word(seed, i));
            a += 4;
        }
        // SAFETY: ROM cache maintenance on a range of the mapped window.
        let rc = unsafe { map::cache_writeback_invalidate(CACHED_BASE + off, CHUNK) };
        if rc != 0 {
            pl!("psram: cache writeback returned {rc}");
        }
        let mut a = 0;
        while a < CHUNK {
            let i = ((off + a) / 4) as u32;
            truth.check(UNC + off + a, patterns::prng_word(seed, i), r(UNC + off + a));
            a += 4;
        }
        let mut a = 0;
        while a < CHUNK {
            let i = ((off + a) / 4) as u32;
            again.check(CACHED_BASE + off + a, patterns::prng_word(seed, i), r(CACHED_BASE + off + a));
            a += 4;
        }
        off += CHUNK;
    }
    let a = verdict(
        format_args!(
            "random fill {seed:#x}, cached write, read back uncached (the memory itself), {} MB",
            TEST_BYTES >> 20
        ),
        &truth,
    );
    let b =
        verdict(format_args!("random fill {seed:#x}, same chunks read back cached, {} MB", TEST_BYTES >> 20), &again);
    a & b
}

/// Fill 1 MB, leave it alone for `secs` seconds (the #569 failure needed 3.5 to 5 s unread), read it back.
fn retention(secs: u32) -> bool {
    const LEN: usize = 1 << 20;
    let mut t = Tally::new();
    let mut a = 0;
    while a < LEN {
        w(UNC + a, patterns::prng_word(0xBEEF_0569, (a / 4) as u32));
        a += 4;
    }
    for _ in 0..secs {
        delay_us(1_000_000);
    }
    let mut a = 0;
    while a < LEN {
        t.check(UNC + a, patterns::prng_word(0xBEEF_0569, (a / 4) as u32), r(UNC + a));
        a += 4;
    }
    verdict(format_args!("retention, 1 MB unread for {secs} s"), &t)
}

/// The cache controller as the ROM left it, then one cached read and one cached write with a line before each, so a
/// hang shows where it is (the L2 tags are undefined after a download-mode boot until `cache_invalidate_all_l2`).
fn cache_diag() {
    const CACHE: usize = 0x3FF1_0000;
    for (n, off) in [
        ("l1i_ctrl", 0x00usize),
        ("l1d_ctrl", 0x04),
        ("l1_bypass", 0x08),
        ("l1d_size", 0x18),
        ("l1d_block", 0x1C),
        ("l1_freeze", 0x2C),
        ("l2_ctrl", 0x270),
        ("l2_bypass", 0x274),
        ("l2_size", 0x278),
        ("l2_block", 0x27C),
        ("l2_freeze", 0x28C),
    ] {
        // SAFETY: cache controller registers, reads.
        pl!("psram: cache {n} {:#010x}", unsafe { lichen_platform_esp32p4::mmio::read32(CACHE + off) });
    }
    lichen_platform_esp32p4::uart::drain();
    pl!("psram: cached read at {CACHED_BASE:#x} ...");
    lichen_platform_esp32p4::uart::drain();
    let v = r(CACHED_BASE + 0x20000);
    pl!("psram: cached read ok: {v:#010x}; cached write ...");
    lichen_platform_esp32p4::uart::drain();
    w(CACHED_BASE + 0x20000, 0x1234_5678);
    pl!("psram: cached write ok; readback {:#010x}", r(CACHED_BASE + 0x20000));
    lichen_platform_esp32p4::uart::drain();
}

/// Stress words of `len` bytes at `off`: the address pattern (its 0x...3fc -> 0x...400 steps switch ten data bits at
/// once, the case that failed at IDF's tuning point) and a checkerboard (all 32 bits switch on every word).
fn stress_bad(off: usize, len: usize) -> u32 {
    let mut bad = 0u32;
    let mut a = 0;
    while a < len {
        w(UNC + off + a, patterns::addr_word((off + a) as u32, false));
        a += 4;
    }
    let mut a = 0;
    while a < len {
        bad += u32::from(r(UNC + off + a) != patterns::addr_word((off + a) as u32, false));
        a += 4;
    }
    let cb = |a: usize| if a & 4 == 0 { 0x5555_5555u32 } else { 0xAAAA_AAAA };
    let mut a = 0;
    while a < len {
        w(UNC + off + a, cb(a));
        a += 4;
    }
    let mut a = 0;
    while a < len {
        bad += u32::from(r(UNC + off + a) != cb(a));
        a += 4;
    }
    bad
}

/// The tuning eye at 120 MHz: for each DQS phase and each delay-line setting of the IDF sweep, how many stress words
/// read back wrong. One line per phase, 31 counts (delay line ids 0..30, see tune::delayline).
pub fn eye() {
    use lichen_platform_esp32p4::psram::{regs, tune};
    // SAFETY: ROM cache maintenance; L2 holds external memory only; the 32 MB are mapped for the stress region.
    unsafe {
        map::cache_invalidate_all_l2();
        map::map_identity_with_hole(PSRAM_BYTES / PAGE_BYTES);
    }
    let (off, len) = (0x50_0000, 0x20_0000);
    pl!("psram: eye: stress bad-word counts over {len:#x} bytes at {off:#x}; rows = DQS phase 0..3, columns = delay line id 0..30");
    for phase in 0..4u32 {
        let mut row = [0u32; tune::DELAYLINES];
        for (i, cell) in row.iter_mut().enumerate() {
            let (data, dqs) = tune::delayline(i);
            // SAFETY: MSPI IOMUX tuning registers, PSRAM bring-up probe only.
            unsafe {
                regs::set_dqs_phase(phase);
                regs::set_delaylines(data, dqs);
            }
            *cell = stress_bad(off, len).min(9999);
        }
        pl!("psram: eye phase {phase}: {row:?}");
    }
}

// ---- diag: where do the 120 MHz flips come from? ------------------------------------------------------------------
// Per case: write the region once, read it three times (right away, again, after DELAY_S seconds), and print the bad
// addresses of the first read. The same set again = stored wrong at write time; a changing set = read side; a growing
// set = retention. Region 0x400000..0x800000 (4 MB) holds the addresses that failed in the full 32 MB pass.
const D_OFF: usize = 0x40_0000;
const D_LEN: usize = 0x40_0000;
const DELAY_S: u32 = 6;

#[derive(Clone, Copy)]
struct BadSet {
    count: u32,
    addrs: [u32; 32],
    bits: [u32; 32],
}
impl BadSet {
    fn new() -> Self {
        BadSet { count: 0, addrs: [0; 32], bits: [0; 32] }
    }
    fn add(&mut self, addr: usize, expected: u32, got: u32) {
        if (self.count as usize) < self.addrs.len() {
            self.addrs[self.count as usize] = addr as u32;
        }
        self.count += 1;
        for (b, n) in self.bits.iter_mut().enumerate() {
            *n += ((expected ^ got) >> b) & 1;
        }
    }
    fn contains(&self, a: u32) -> bool {
        self.addrs.iter().take(self.count.min(32) as usize).any(|&x| x == a)
    }
    /// How many of this set's recorded addresses are also in `other`.
    fn shared(&self, other: &BadSet) -> u32 {
        self.addrs.iter().take(self.count.min(32) as usize).filter(|&&a| other.contains(a)).count() as u32
    }
}

fn fifo_read_word(off: usize, buf: &mut [u32; 16], base: &mut usize) -> u32 {
    use lichen_platform_esp32p4::psram::mspi;
    let blk = off & !63;
    if *base != blk {
        // SAFETY: FIFO-path read on the initialised controller (variable dummy off, see `diag`).
        unsafe { mspi::read_data(buf, blk as u32) };
        *base = blk;
    }
    buf[(off & 63) / 4]
}

#[derive(Clone, Copy, PartialEq)]
enum Rd {
    Axi,
    Fifo,
}
fn read_region(f: &dyn Fn(usize) -> u32, how: Rd) -> BadSet {
    let mut set = BadSet::new();
    let (mut buf, mut base) = ([0u32; 16], usize::MAX);
    let mut a = 0;
    while a < D_LEN {
        let off = D_OFF + a;
        let g = match how {
            Rd::Axi => r(UNC + off),
            Rd::Fifo => fifo_read_word(off, &mut buf, &mut base),
        };
        let e = f(off);
        if g != e {
            set.add(off, e, g);
        }
        a += 4;
    }
    set
}
fn show_set(tag: &str, s: &BadSet, other: Option<&BadSet>) {
    pl!("psram: diag   {tag}: {} bad", s.count);
    if let Some(o) = other {
        pl!("psram: diag     {} of its first {} bad addresses are also bad in read 1", s.shared(o), s.count.min(32));
    }
}
fn show_addrs(s: &BadSet) {
    for &a in s.addrs.iter().take(s.count.min(16) as usize) {
        pl!("psram: diag     bad at {a:#x}");
    }
    for (b, &n) in s.bits.iter().enumerate() {
        if n != 0 {
            pl!("psram: diag     bit {b}: {n} words");
        }
    }
}

fn diag_case(name: &str, f: &dyn Fn(usize) -> u32, write_cached: bool, read_how: Rd, retention: bool) {
    pl!("psram: diag {name}");
    let mut a = 0;
    while a < D_LEN {
        let off = D_OFF + a;
        let base = if write_cached { CACHED_BASE } else { UNC };
        w(base + off, f(off));
        a += 4;
    }
    if write_cached {
        // SAFETY: ROM cache maintenance over the written range.
        let rc = unsafe { map::cache_writeback_invalidate(CACHED_BASE + D_OFF, D_LEN) };
        if rc != 0 {
            pl!("psram: diag   writeback returned {rc}");
        }
    }
    let r1 = read_region(f, read_how);
    show_set("read 1 (right after the write)", &r1, None);
    show_addrs(&r1);
    let r2 = read_region(f, read_how);
    show_set("read 2 (again)", &r2, Some(&r1));
    if retention {
        for _ in 0..DELAY_S {
            delay_us(1_000_000);
        }
        let r3 = read_region(f, read_how);
        show_set("read 3 (after the 6 s wait)", &r3, Some(&r1));
    }
}

/// The cached alias really is cached: a cached write is not in PSRAM until written back.
fn cache_proof() {
    let at = 0x60_0000usize;
    w(UNC + at, 0x1111_1111);
    w(CACHED_BASE + at, 0x2222_2222);
    let before = r(UNC + at);
    // SAFETY: ROM cache maintenance.
    unsafe { map::cache_writeback_invalidate(CACHED_BASE + at, 64) };
    let after = r(UNC + at);
    pl!("psram: diag cache proof: PSRAM holds {before:#010x} before write-back (expect 0x11111111), {after:#010x} after (expect 0x22222222)");
}

/// Case F: the same address pattern, but each single-word write is followed by a pause, so writes never queue up.
fn diag_spaced() {
    pl!("psram: diag F address, AXI single-word writes spaced by a pause (no write queueing), AXI read");
    let mut a = 0;
    while a < D_LEN {
        w(UNC + D_OFF + a, (D_OFF + a) as u32);
        lichen_platform_esp32p4::uart::spin(60);
        a += 4;
    }
    let r1 = read_region(&|a| a as u32, Rd::Axi);
    show_set("read 1", &r1, None);
    show_addrs(&r1);
}

/// Case G: single words at addresses that failed in case A, written alone (long pause after each), 20 times each.
fn diag_isolated(known: &BadSet) {
    pl!("psram: diag G isolated writes at the {} addresses that failed in case A, 20 tries each (write value = address)", known.count.min(32));
    let mut total_fail = 0;
    for &a in known.addrs.iter().take(known.count.min(32) as usize) {
        let mut fails = 0;
        for _ in 0..20 {
            w(UNC + a as usize, a);
            lichen_platform_esp32p4::uart::spin(2000);
            if r(UNC + a as usize) != a {
                fails += 1;
            }
        }
        total_fail += fails;
        if fails != 0 {
            pl!("psram: diag   {a:#x}: {fails} of 20 isolated writes stored wrong");
        }
    }
    pl!("psram: diag   isolated writes stored wrong: {total_fail} of {}", 20 * known.count.min(32));
}

pub fn diag() {
    use lichen_platform_esp32p4::psram::regs;
    // SAFETY: as `eye`.
    unsafe {
        map::cache_invalidate_all_l2();
        map::map_identity_with_hole(PSRAM_BYTES / PAGE_BYTES);
        regs::apply(&[regs::VAR_DUMMY_FIFO_OFF]); // FIFO-path reads were tuned with the variable dummy off
    }
    pl!("psram: diag: region {D_OFF:#x}..{:#x}", D_OFF + D_LEN);
    cache_proof();
    diag_case("A address, AXI single-word write, AXI read", &|a| a as u32, false, Rd::Axi, false);
    diag_case("D address, AXI write, FIFO read (re-check)", &|a| a as u32, false, Rd::Fifo, false);
    diag_spaced();
    // the failing set of the queued-write case, rewritten and re-read, for case G
    let mut a = 0;
    while a < D_LEN {
        w(UNC + D_OFF + a, (D_OFF + a) as u32);
        a += 4;
    }
    let known = read_region(&|a| a as u32, Rd::Axi);
    pl!("psram: diag baseline (queued writes again): {} bad", known.count);
    diag_isolated(&known);
    diag_case("E address, cached write + write-back, AXI read", &|a| a as u32, true, Rd::Axi, false);
}

static PERF_BYTES: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
static PERF_ERRORS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

unsafe fn mcycle() -> u32 {
    let v: u32;
    core::arch::asm!("csrr {}, mcycle", out(reg) v);
    v
}

/// Sequential throughput through the cached alias, 4 MB: read (line fills), write plus write-back, as MB/s from the cycle
/// counter and the measured CPU clock.
pub fn perf(cpu_mhz: u32) {
    const LEN: usize = 4 << 20;
    // SAFETY: ROM cache maintenance; mapped window.
    unsafe {
        map::cache_invalidate_all_l2();
        map::map_identity_with_hole(PSRAM_BYTES / PAGE_BYTES);
        map::cache_invalidate(CACHED_BASE, PSRAM_BYTES);
    }
    pl!("PERF psram.clock mhz={}", lichen_platform_esp32p4::psram::current_mhz());
    let mhz = if cpu_mhz == 0 { 40 } else { cpu_mhz } as u64;
    let rate = |cycles: u32| -> u64 { LEN as u64 * mhz / cycles.max(1) as u64 }; // bytes per us = MB/s
                                                                                 // SAFETY: cycle counter reads.
    let c0 = unsafe { mcycle() };
    let mut a = 0;
    let mut sink = 0u32;
    while a < LEN {
        sink ^= r(CACHED_BASE + 0x100_0000 + a);
        a += 4;
    }
    let c1 = unsafe { mcycle() };
    let mut a = 0;
    while a < LEN {
        w(CACHED_BASE + 0x180_0000 + a, a as u32 ^ sink);
        a += 4;
    }
    // SAFETY: ROM cache maintenance.
    unsafe { map::cache_writeback_invalidate(CACHED_BASE + 0x180_0000, LEN) };
    let c2 = unsafe { mcycle() };
    pl!("PERF psram.seq_read mb_s={} cpu_mhz={mhz}", rate(c1.wrapping_sub(c0)));
    pl!("PERF psram.seq_write mb_s={} cpu_mhz={mhz}", rate(c2.wrapping_sub(c1)));
}

/// Everything. Returns the number of failed tests.
pub fn run(cpu_mhz: u32) -> u32 {
    let mut fails = 0;
    // SAFETY: ROM cache maintenance; L2 holds external memory only.
    let rc_l2 = unsafe { map::cache_invalidate_all_l2() };
    pl!("psram: L2 invalidate all returned {rc_l2}");
    // SAFETY: `init` reached Tuned; 512 pages cover the 32 MB the chip reports.
    unsafe { map::map_identity_with_hole(PSRAM_BYTES / PAGE_BYTES) };
    // SAFETY: ROM cache maintenance over the freshly mapped window.
    let rc = unsafe { map::cache_invalidate(CACHED_BASE, PSRAM_BYTES) };
    pl!(
        "psram: mapped {} pages at {CACHED_BASE:#x} (uncached alias {UNC:#x}), cache invalidate returned {rc}",
        PSRAM_BYTES / PAGE_BYTES
    );
    cache_diag();
    fails += u32::from(!walking(UNC, "walking bits, uncached alias"));
    fails += u32::from(!walking(CACHED_BASE, "walking bits, cached alias (no write-back: stays in the cache)"));
    fails += u32::from(!addr_in_addr_uncached());
    fails += u32::from(!random_cached(0x1234_5678));
    fails += u32::from(!random_cached(0xA5A5_5A5A));
    fails += u32::from(!retention(6));
    pl!(
        "PERF psram.pattern errors={} bytes={}",
        PERF_ERRORS.load(core::sync::atomic::Ordering::Relaxed),
        PERF_BYTES.load(core::sync::atomic::Ordering::Relaxed)
    );
    perf(cpu_mhz);
    pl!(
        "CHECK 620.pattern {} at {} MHz, cpu {} MHz",
        if fails == 0 { "PASS" } else { "FAIL" },
        lichen_platform_esp32p4::psram::current_mhz(),
        cpu_mhz
    );
    fails
}
