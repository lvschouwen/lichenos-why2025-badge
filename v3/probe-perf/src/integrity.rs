//! Memory integrity over the whole chip, the hole included (v2's flips sat in 0x55xxxx-0x62xxxx at 0x400 mod 0x800). Dense, inverted,
//! multi-pass patterns (`lichen_perflab::patterns::PASSES`), written only through the cached alias or by DMA; every read-back is
//! through the cache after the lines were dropped, so it fetches what the chip holds. Nothing here stores through the uncached alias.
use crate::con::{check, pass_if, perf};
use crate::dmabench::xfer;
use crate::hw::{ticks, wait_us, TICK_HZ};
use crate::mem::*;
use core::fmt::{self, Write};
use lichen_kcore::check::Verdict;
use lichen_perflab::line::Val;
use lichen_perflab::patterns::{Pattern, Tally, PASSES, WATCH_END, WATCH_START};
use lichen_perflab::rate::mb_s_x100;

const CHUNK: usize = 64 << 10;

/// A metric or detail name built at run time (no allocator).
struct Name([u8; 64], usize);
impl Write for Name {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let b = s.as_bytes();
        let end = self.1 + b.len();
        if end > self.0.len() {
            return Err(fmt::Error);
        }
        self.0[self.1..end].copy_from_slice(b);
        self.1 = end;
        Ok(())
    }
}
impl Name {
    fn new(kind: &str, p: Pattern) -> Name {
        let mut n = Name([0; 64], 0);
        let _ = write!(n, "lab.integrity.{kind}.{}", p.name());
        n
    }
    fn as_str(&self) -> &str {
        core::str::from_utf8(&self.0[..self.1]).unwrap_or("lab.integrity.bad")
    }
}

fn fill_cached(p: Pattern) {
    let mut off = 0;
    while off < CHIP {
        let base = cached(off);
        let mut a = 0;
        while a < CHUNK {
            wr(base + a, p.word((off + a) as u32));
            a += 4;
        }
        wb_inv(base, CHUNK);
        off += CHUNK;
    }
}

/// Read the whole chip through the cache and compare.
fn verify_cached(p: Pattern) -> Tally {
    let mut t = Tally::new();
    let mut off = 0;
    while off < CHIP {
        let base = cached(off);
        let mut a = 0;
        while a < CHUNK {
            t.check((off + a) as u32, p.word((off + a) as u32), rd(base + a));
            a += 4;
        }
        off += CHUNK;
    }
    t
}

fn fill_buf(p: Pattern, off: usize) {
    let b = buf_a();
    let mut a = 0;
    while a < BUF {
        wr(b + a, p.word((off + a) as u32));
        a += 4;
    }
}

/// Write the chip from internal RAM by DMA: the destination is clean and empty first, the source written back first.
fn fill_dma(p: Pattern) -> Result<(), Name> {
    wb_inv(cached(0), CHIP);
    let mut steps = 0;
    let mut off = 0;
    while off < CHIP {
        fill_buf(p, off);
        wb_inv(buf_a(), BUF);
        xfer(buf_a(), cached(off), BUF, &mut steps).map_err(|f| fault(off, f))?;
        off += BUF;
    }
    inv(cached(0), CHIP); // the DMA wrote the chip behind the cache: drop whatever the cache holds
    Ok(())
}

/// Read the chip back by DMA into internal RAM and compare there.
fn verify_dma(p: Pattern) -> Result<Tally, Name> {
    let (mut t, mut steps, mut off) = (Tally::new(), 0, 0);
    while off < CHIP {
        wb_inv(cached(off), BUF);
        wb_inv(buf_b(), BUF);
        xfer(cached(off), buf_b(), BUF, &mut steps).map_err(|f| fault(off, f))?;
        inv(buf_b(), BUF); // the DMA wrote the buffer behind the cache
        let mut a = 0;
        while a < BUF {
            t.check((off + a) as u32, p.word((off + a) as u32), rd(buf_b() + a));
            a += 4;
        }
        off += BUF;
    }
    Ok(t)
}

fn fault(off: usize, f: crate::dmabench::Poll) -> Name {
    let mut n = Name([0; 64], 0);
    let _ = write!(n, "dma fault at {off:#x}: {f:?}");
    n
}

fn label(kind: &str, p: Pattern) -> Name {
    let mut n = Name([0; 64], 0);
    let _ = write!(n, "{kind} {}", p.name());
    n
}

/// Print what failed: the first words, the bits that flipped and in which direction, and the v2 watch class.
fn show_fail(what: &str, t: &Tally) {
    check(
        "648.integrity",
        Verdict::Fail,
        format_args!(
            "{what}: {} bad of {} words, {} in the watch class (0x400 mod 0x800 in {WATCH_START:#x}-{WATCH_END:#x})",
            t.bad, t.words, t.watch
        ),
    );
    for &(off, e, g) in t.first.iter().take(t.bad.min(6) as usize) {
        pl!("integrity:   at {off:#010x}: expected {e:#010x} got {g:#010x}");
    }
    for b in 0..32 {
        if t.up[b] + t.down[b] != 0 {
            pl!("integrity:   bit {b}: {} words 0->1, {} words 1->0", t.up[b], t.down[b]);
        }
    }
}

fn report(kind: &str, p: Pattern, t: &Tally, ticks_used: u64) {
    let name = Name::new(kind, p);
    perf(
        name.as_str(),
        &[
            ("mb_s", Val::F2(mb_s_x100(2 * CHIP as u64, ticks_used, TICK_HZ) as i64)),
            ("words", Val::U(t.words)),
            ("bad", Val::U(t.bad)),
            ("watch", Val::U(t.watch)),
        ],
    );
}

fn cached_pass(p: Pattern) -> bool {
    let t0 = ticks();
    fill_cached(p);
    let t = verify_cached(p);
    report("cached", p, &t, ticks() - t0);
    if !t.clean() {
        show_fail(label("cached", p).as_str(), &t);
    }
    t.clean()
}

fn dma_pass(p: Pattern) -> bool {
    let t0 = ticks();
    let res = fill_dma(p).and_then(|()| {
        let w = verify_cached(p); // the DMA write path, read back by the CPU
        let r = verify_dma(p)?; // the DMA read path
        Ok((w, r))
    });
    match res {
        Err(why) => {
            check("648.integrity", Verdict::Fail, format_args!("dma {}: {}", p.name(), why.as_str()));
            false
        }
        Ok((w, r)) => {
            let used = ticks() - t0;
            report("dma_write", p, &w, used);
            report("dma_read", p, &r, used);
            if !w.clean() {
                show_fail(label("dma write, cpu read-back", p).as_str(), &w);
            }
            if !r.clean() {
                show_fail(label("dma read-back", p).as_str(), &r);
            }
            w.clean() && r.clean()
        }
    }
}

/// Rounds of cached and DMA passes over every pattern until `budget_s` seconds are used (always at least the first pair).
/// Returns false at the first flip or fault: the experiment stops there and records a failed point.
pub fn run(budget_s: u32, after_pass: &mut dyn FnMut() -> bool) -> bool {
    crate::con::begin("integrity");
    pl!("integrity: whole chip {} MB, hole 0x540000-0x5C0000 included, watch class 0x400 mod 0x800 in {WATCH_START:#x}-{WATCH_END:#x}", CHIP >> 20);
    let (t0, budget) = (ticks(), budget_s as u64 * TICK_HZ);
    let mut passes = 0u32;
    'rounds: loop {
        for p in PASSES {
            if !(cached_pass(p) && dma_pass(p)) || !after_pass() {
                return false;
            }
            passes += 1;
            if ticks() - t0 >= budget {
                break 'rounds;
            }
        }
    }
    let fails = CACHE_FAILS.load(core::sync::atomic::Ordering::Relaxed);
    pass_if(
        fails == 0,
        "648.integrity",
        format_args!(
            "{passes} pattern passes, each cached and DMA, whole chip, 0 bad words ({fails} cache sync failures)"
        ),
    );
    fails == 0
}

/// The #569 style park: data written by the CPU through the cache and data written by DMA sit untouched for `secs` seconds, then
/// are read back through the cache. 1 MB each; the first region holds 0xC4AB4C, the address of the #569 flip.
pub fn park(secs: u32) -> bool {
    const LEN: usize = 1 << 20;
    crate::con::begin("park");
    let o = &PSRAM_OFFSETS;
    let (cpu_p, dma_p) = (Pattern::Rand(0x0569_0001), Pattern::Rand(0x0569_0002));
    let dma_off = o.park + 0x20_0000;
    let mut a = 0;
    while a < LEN {
        wr(cached(o.park + a), cpu_p.word((o.park + a) as u32));
        a += 4;
    }
    wb_inv(cached(o.park), LEN);
    let mut steps = 0;
    wb_inv(cached(dma_off), LEN);
    let mut off = 0;
    while off < LEN {
        fill_buf(dma_p, dma_off + off);
        wb_inv(buf_a(), BUF);
        if xfer(buf_a(), cached(dma_off + off), BUF, &mut steps).is_err() {
            check("648.park", Verdict::Fail, format_args!("dma fault while filling"));
            return false;
        }
        off += BUF;
    }
    inv(cached(dma_off), LEN);
    for _ in 0..secs {
        wait_us(1_000_000);
    }
    let (mut t_cpu, mut t_dma) = (Tally::new(), Tally::new());
    let mut a = 0;
    while a < LEN {
        t_cpu.check((o.park + a) as u32, cpu_p.word((o.park + a) as u32), rd(cached(o.park + a)));
        t_dma.check((dma_off + a) as u32, dma_p.word((dma_off + a) as u32), rd(cached(dma_off + a)));
        a += 4;
    }
    for (what, t) in [("cpu-written", &t_cpu), ("dma-written", &t_dma)] {
        if !t.clean() {
            show_fail(what, t);
        }
    }
    let ok = t_cpu.clean() && t_dma.clean();
    pass_if(
        ok,
        "648.park",
        format_args!(
            "{secs} s untouched, 2 x {} KB (cpu-written and dma-written), {} + {} bad words",
            LEN >> 10,
            t_cpu.bad,
            t_dma.bad
        ),
    );
    ok
}
