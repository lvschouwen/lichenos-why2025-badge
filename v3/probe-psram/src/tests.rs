//! Pattern tests of the 32 MB PSRAM at 120 MHz, through the cached and the uncached alias.
//!
//! Layout: `map_identity` puts physical page n at virtual page n. The uncached alias is the memory truth (every access
//! is a PSRAM transaction); the cached alias goes through the L1/L2 caches, so a cached test writes, writes the lines
//! back, and reads the truth through the uncached alias. Failures print the first few addresses and a per-bit count of
//! flipped bits (the #86 / #569 history: bits 8 and 9, 0 -> 1).
use core::ptr::{read_volatile, write_volatile};
use lichen_platform_esp32p4::psram::map::{self, CACHED_BASE, PAGE_BYTES, UNCACHED_OFFSET};
use lichen_platform_esp32p4::psram::{mspi::delay_us, patterns};

pub const PSRAM_BYTES: usize = 32 << 20;
const UNC: usize = CACHED_BASE + UNCACHED_OFFSET;
const CHUNK: usize = PAGE_BYTES; // 64 KB: fits the caches, so a chunk is written back and verified in one go

struct Tally {
    words: u64,
    bad: u64,
    bit_flips: [u32; 32],
    first: [(usize, u32, u32); 4],
}
impl Tally {
    fn new() -> Self {
        Tally { words: 0, bad: 0, bit_flips: [0; 32], first: [(0, 0, 0); 4] }
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
        for &(a, e, g) in t.first.iter().take(t.bad.min(4) as usize) {
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
        if base == CACHED_BASE {
            pl!("psram: walking cached at {a:#010x}");
            lichen_platform_esp32p4::uart::drain();
        }
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
        let mut a = 0;
        while a < PSRAM_BYTES {
            w(UNC + a, patterns::addr_word(a as u32, inverted));
            a += 4;
        }
        let mut a = 0;
        while a < PSRAM_BYTES {
            t.check(UNC + a, patterns::addr_word(a as u32, inverted), r(UNC + a));
            a += 4;
        }
        ok &= verdict(
            format_args!("address-in-address, uncached, {}, 32 MB", if inverted { "inverted" } else { "plain" }),
            &t,
        );
    }
    ok
}

/// A pseudo-random fill of the whole window through the cached alias, a chunk at a time: write cached, write the lines
/// back, read the truth through the uncached alias; then drop the lines and read the same chunk cached again.
fn random_cached(seed: u32) -> bool {
    let (mut truth, mut again) = (Tally::new(), Tally::new());
    let mut off = 0;
    while off < PSRAM_BYTES {
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
        format_args!("random fill {seed:#x}, cached write, read back uncached (the memory itself), 32 MB"),
        &truth,
    );
    let b = verdict(format_args!("random fill {seed:#x}, same chunks read back cached, 32 MB"), &again);
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
/// hang shows where it is.
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
    for a in [0x0usize, 0xFFFC, 0x1_0000, 0x1_0040, 0x1_1000, 0x2_0000] {
        pl!("psram: step read cached +{a:#x}");
        lichen_platform_esp32p4::uart::drain();
        let v = r(CACHED_BASE + a);
        pl!("psram: step write cached +{a:#x} (read {v:#010x})");
        lichen_platform_esp32p4::uart::drain();
        w(CACHED_BASE + a, 0xA5A5_0000 + a as u32);
    }
    pl!("psram: steps done");
    lichen_platform_esp32p4::uart::drain();
}

/// Everything. Returns the number of failed tests.
pub fn run() -> u32 {
    let mut fails = 0;
    // SAFETY: ROM cache maintenance; L2 holds external memory only.
    let rc_l2 = unsafe { map::cache_invalidate_all_l2() };
    pl!("psram: L2 invalidate all returned {rc_l2}");
    // SAFETY: `init` reached Tuned; 512 pages cover the 32 MB the chip reports.
    unsafe { map::map_identity(PSRAM_BYTES / PAGE_BYTES) };
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
    fails
}
