//! Memory throughput and latency, internal RAM as the ceiling and PSRAM through the cached alias (uncached writes are out of scope
//! for #648). Rates are MB/s from SYSTIMER ticks; latency is the pointer chase over a ring with a 128 B stride, so every hop is a new
//! line whatever the L2 line size is.
use crate::con::{check, perf};
use crate::hw::{ticks, TICK_HZ};
use crate::mem::*;
use core::hint::black_box;
use lichen_kcore::check::Verdict;
use lichen_perflab::chase::sattolo;
use lichen_perflab::line::Val;
use lichen_perflab::rate::{mb_s_x100, mops_x100, ns_x100};
use lichen_perflab::rng::Rng;
use lichen_platform_esp32p4::psram::map::CACHED_BASE;

const SEQ: usize = 4 << 20;

#[inline(never)]
pub(crate) fn read_words(base: usize, words: usize) -> u32 {
    let (mut s, mut i) = (0u32, 0);
    while i < words {
        let a = base + i * 4;
        s ^= rd(a) ^ rd(a + 4) ^ rd(a + 8) ^ rd(a + 12) ^ rd(a + 16) ^ rd(a + 20) ^ rd(a + 24) ^ rd(a + 28);
        i += 8;
    }
    s
}
#[inline(never)]
pub(crate) fn write_words(base: usize, words: usize, v: u32) {
    let mut i = 0;
    while i < words {
        let a = base + i * 4;
        for k in 0..8 {
            wr(a + k * 4, v ^ (i + k) as u32);
        }
        i += 8;
    }
}
#[inline(never)]
fn copy_words(dst: usize, src: usize, words: usize) {
    let mut i = 0;
    while i < words {
        let (d, s) = (dst + i * 4, src + i * 4);
        for k in 0..8 {
            wr(d + k * 4, rd(s + k * 4));
        }
        i += 8;
    }
}
#[inline(never)]
fn random_reads(base: usize, words: u32, n: u32, rng: &mut Rng) -> u32 {
    let mut s = 0u32;
    for _ in 0..n {
        s ^= rd(base + rng.below(words) as usize * 4);
    }
    s
}
/// `hops` dependent loads along a ring that holds each next address.
#[inline(never)]
fn chase(start: usize, hops: u32) -> usize {
    let mut p = start;
    let mut i = 0;
    while i < hops {
        p = rd(p) as usize;
        p = rd(p) as usize;
        p = rd(p) as usize;
        p = rd(p) as usize;
        i += 4;
    }
    p
}

fn rate(metric: &str, bytes: u64, t: u64) {
    perf(metric, &[("mb_s", Val::F2(mb_s_x100(bytes, t, TICK_HZ) as i64))]);
}

/// The same loops on internal RAM, repeated over the two buffers (L1-resident: the ceiling for the CPU path).
fn sram() {
    let (a, b, words) = (buf_a(), buf_b(), BUF / 4);
    let reps = SEQ / BUF;
    write_words(a, words, 1);
    let t0 = ticks();
    let mut s = 0;
    for _ in 0..reps {
        s ^= read_words(a, words);
    }
    let t1 = ticks();
    for _ in 0..reps {
        write_words(a, words, s);
    }
    let t2 = ticks();
    for _ in 0..reps {
        copy_words(b, a, words);
    }
    let t3 = ticks();
    black_box(s);
    rate("lab.mem.sram.seq_read", SEQ as u64, t1 - t0);
    rate("lab.mem.sram.seq_write", SEQ as u64, t2 - t1);
    rate("lab.mem.sram.copy", SEQ as u64, t3 - t2);
}

fn psram_seq() {
    let o = &PSRAM_OFFSETS;
    for off in [o.read, o.write, o.copy_dst] {
        wb_inv(cached(off), SEQ);
    }
    let t0 = ticks();
    let s = read_words(cached(o.read), SEQ / 4);
    let t1 = ticks();
    write_words(cached(o.write), SEQ / 4, s);
    let t2 = ticks();
    wb_inv(cached(o.write), SEQ);
    let t3 = ticks();
    copy_words(cached(o.copy_dst), cached(o.read), SEQ / 4);
    wb_inv(cached(o.copy_dst), SEQ);
    let t4 = ticks();
    black_box(s);
    rate("lab.mem.psram.seq_read", SEQ as u64, t1 - t0);
    // the stores only (line fills for write allocation and evictions included) and the whole write with the write-back
    perf(
        "lab.mem.psram.seq_write",
        &[
            ("mb_s", Val::F2(mb_s_x100(SEQ as u64, t3 - t1, TICK_HZ) as i64)),
            ("store_mb_s", Val::F2(mb_s_x100(SEQ as u64, t2 - t1, TICK_HZ) as i64)),
        ],
    );
    rate("lab.mem.psram.copy", SEQ as u64, t4 - t3);
}

fn psram_random() {
    const WINDOW: usize = 8 << 20;
    const N: u32 = 262_144;
    let base = cached(PSRAM_OFFSETS.chase);
    wb_inv(base, WINDOW);
    let t0 = ticks();
    let s = random_reads(base, (WINDOW / 4) as u32, N, &mut Rng::new(0x1234_5678));
    let t1 = ticks();
    black_box(s);
    perf(
        "lab.mem.psram.rand_read",
        &[
            ("mops", Val::F2(mops_x100(N as u64, t1 - t0, TICK_HZ) as i64)),
            ("ns", Val::F2(ns_x100(t1 - t0, N as u64, TICK_HZ) as i64)),
        ],
    );
}

/// Link `slots` slots of `stride` bytes at `base` into one random ring (next address stored in each slot), `perm` as scratch.
fn build_ring(base: usize, stride: usize, perm: &mut [u32], rng: &mut Rng) {
    sattolo(perm, rng);
    for (i, &next) in perm.iter().enumerate() {
        wr(base + i * stride, (base + next as usize * stride) as u32);
    }
}

/// Every slot must hold the address `perm` gives it. A mismatch after the cache was written back is a cached-path corruption (a failed
/// point); the memory itself is read through the uncached alias to tell a lost write from a bad read.
fn verify_ring(metric: &str, base: usize, stride: usize, perm: &[u32]) -> bool {
    let (mut bad, mut first) = (0u32, [(0usize, 0u32, 0u32); 3]);
    for (i, &next) in perm.iter().enumerate() {
        let (exp, got) = ((base + next as usize * stride) as u32, rd(base + i * stride));
        if exp != got {
            if (bad as usize) < first.len() {
                first[bad as usize] = (i, exp, got);
            }
            bad += 1;
        }
    }
    if bad != 0 {
        check("648.mem.ring", Verdict::Fail, format_args!("{metric}: {bad} of {} slots read back wrong", perm.len()));
        for &(i, e, g) in first.iter().take(bad.min(3) as usize) {
            let off = base - CACHED_BASE + i * stride;
            pl!("mem:   slot {i} at {off:#x}: expected {e:#010x} cached read {g:#010x} memory {:#010x}", rd_truth(off));
        }
    }
    bad == 0
}

/// Build a ring, check it, and time `hops` dependent loads along it. Returns ns per hop in hundredths, or `None` if the ring read back
/// wrong (already reported).
fn chase_measure(metric: &str, base: usize, stride: usize, ws: usize, perm: &mut [u32], cold: bool) -> Option<u64> {
    let slots = ws / stride;
    let perm = &mut perm[..slots];
    build_ring(base, stride, perm, &mut Rng::new(ws as u32 ^ stride as u32));
    if cold {
        wb_inv(base, ws); // the build's stores reach the chip and the cache starts empty
    }
    if !verify_ring(metric, base, stride, perm) {
        return None;
    }
    let hops = 200_000u32;
    // two laps (a ring that fits a cache is then resident; one that does not stays a stream of misses)
    black_box(chase(base, (slots as u32 * 2).min(hops).next_multiple_of(4)));
    let t0 = ticks();
    let p = chase(base, hops);
    let t1 = ticks();
    black_box(p);
    Some(ns_x100(t1 - t0, hops as u64, TICK_HZ))
}

fn chase_one(metric: &str, base: usize, stride: usize, ws: usize, perm: &mut [u32], cold: bool) -> bool {
    let Some(ns) = chase_measure(metric, base, stride, ws, perm, cold) else { return false };
    perf(metric, &[("ns", Val::F2(ns as i64)), ("stride_b", Val::U(stride as u64))]);
    true
}

/// The PSRAM ring of `ws` bytes, for the other suites: ns per hop in hundredths (0 if the ring was wrong).
pub(crate) fn chase_ring(ws: usize, stride: usize) -> u64 {
    let o = &PSRAM_OFFSETS;
    // SAFETY: the permutation scratch is a cached-alias PSRAM range of 1 MB that nothing else uses.
    let perm = unsafe { core::slice::from_raw_parts_mut(cached(o.perm) as *mut u32, 1 << 18) };
    chase_measure("lab.mem.chase.psram.dual", cached(o.chase), stride, ws, perm, true).unwrap_or(0)
}

fn chases() -> bool {
    const PSRAM_SETS: [(&str, usize, usize); 8] = [
        ("lab.mem.chase.psram.ws8k", 8 << 10, 128),
        ("lab.mem.chase.psram.ws32k", 32 << 10, 128),
        ("lab.mem.chase.psram.ws64k", 64 << 10, 128),
        ("lab.mem.chase.psram.ws128k", 128 << 10, 128),
        ("lab.mem.chase.psram.ws512k", 512 << 10, 128),
        ("lab.mem.chase.psram.ws2m", 2 << 20, 128),
        ("lab.mem.chase.psram.ws8m", 8 << 20, 128),
        ("lab.mem.chase.psram.ws8m_s64", 8 << 20, 64),
    ];
    let o = &PSRAM_OFFSETS;
    // SAFETY: the permutation scratch is a cached-alias PSRAM range of 1 MB that nothing else uses.
    let perm = unsafe { core::slice::from_raw_parts_mut(cached(o.perm) as *mut u32, 1 << 18) };
    for (metric, ws, stride) in PSRAM_SETS {
        if !chase_one(metric, cached(o.chase), stride, ws, perm, true) {
            return false;
        }
    }
    // internal RAM, L1-resident: 128 slots in the first buffer, the permutation in the second
    // SAFETY: the second buffer is 16 KB of u32, nothing else uses it during the test.
    let sperm = unsafe { core::slice::from_raw_parts_mut(buf_b() as *mut u32, BUF / 4) };
    chase_one("lab.mem.chase.sram.ws16k", buf_a(), 128, BUF, sperm, false)
        && chase_one("lab.mem.chase.sram.ws4k", buf_a(), 128, 4 << 10, sperm, false)
}

/// Returns false if a ring read back wrong.
pub fn run() -> bool {
    crate::con::begin("mem");
    sram();
    psram_seq();
    psram_random();
    chases()
}
