//! AXI-GDMA memory-to-memory: throughput, the CPU's cost of starting a transfer, and how much ALU work fits in the wait. The
//! platform's `dma::run` blocks, so the probe starts and polls the channel itself (same registers, same descriptor chains).
use crate::con::{check, perf};
use crate::hw::{mcycle, ticks, TICK_HZ};
use crate::mem::*;
use core::hint::black_box;
use core::ptr::addr_of_mut;
use lichen_kcore::check::Verdict;
use lichen_perflab::line::Val;
use lichen_perflab::rate::{mb_s_x100, pct_x100};
use lichen_platform_esp32p4::mmio::{modify32, read32, write32};
use lichen_platform_esp32p4::psram::dma::{self, Desc, SEG_BYTES};

const NSEG: usize = BUF / SEG_BYTES;
static mut TXD: [Desc; NSEG] = [Desc::zero(); NSEG];
static mut RXD: [Desc; NSEG] = [Desc::zero(); NSEG];

const AXI_DMA: usize = 0x5008_A000;
const RX_INT_RAW: usize = AXI_DMA;
const RX_INT_CLR: usize = AXI_DMA + 0x0C;
const RX_LINK: usize = AXI_DMA + 0x20;
const RX_DESC: usize = AXI_DMA + 0x24;
const TX_INT_RAW: usize = AXI_DMA + 0x138;
const TX_INT_CLR: usize = AXI_DMA + 0x144;
const TX_LINK: usize = AXI_DMA + 0x158;
const TX_DESC: usize = AXI_DMA + 0x15C;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Poll {
    Busy,
    Done,
    /// RX and TX raw interrupt words.
    Fault(u32, u32),
}

/// Build the chains (written back to memory) and start the channel; returns the CPU cycles that took.
///
/// # Safety
/// `dma::init` has run; `src` and `dst` are 128-aligned, `len` a multiple of 128 and at most `BUF`; the caches follow the contract
/// of the platform's `dma.rs` (the caller's job); nothing else uses the channel.
unsafe fn start(src: usize, dst: usize, len: usize) -> u32 {
    let c0 = mcycle();
    let (txd, rxd) = (addr_of_mut!(TXD), addr_of_mut!(RXD));
    dma::build_chain(&mut *txd, src as u32, len, true);
    dma::build_chain(&mut *rxd, dst as u32, len, false);
    // the engine reads the descriptors from memory, not from the CPU's cache (a descriptor error without this)
    wb_inv(txd as usize, NSEG * 16);
    wb_inv(rxd as usize, NSEG * 16);
    write32(RX_INT_CLR, 0xFFFF_FFFF);
    write32(TX_INT_CLR, 0xFFFF_FFFF);
    write32(RX_DESC, rxd as u32);
    write32(TX_DESC, txd as u32);
    modify32(RX_LINK, 0x4, 0x4);
    modify32(TX_LINK, 0x2, 0x2);
    mcycle().wrapping_sub(c0)
}

fn poll() -> Poll {
    // SAFETY: AXI-DMA interrupt status words, reads.
    let (raw, traw) = unsafe { (read32(RX_INT_RAW), read32(TX_INT_RAW)) };
    if raw & 0xC != 0 || traw & 0x4 != 0 || (raw & 0x2 == 0 && raw & 0x10 != 0) {
        Poll::Fault(raw, traw)
    } else if raw & 0x2 != 0 {
        Poll::Done
    } else {
        Poll::Busy
    }
}

/// Run one transfer to the end, counting ALU steps done while it runs into `steps`. Returns the setup cycles, or the fault.
pub fn xfer(src: usize, dst: usize, len: usize, steps: &mut u64) -> Result<u32, Poll> {
    // SAFETY: the contract of `start`, handed on by this function's callers (the suites own the buffers and the caches).
    let setup = unsafe { start(src, dst, len) };
    let (mut x, mut n, mut spins) = (0x1234_5678u32, 0u64, 0u32);
    loop {
        match poll() {
            Poll::Done => break,
            Poll::Busy => {}
            f => {
                // SAFETY: both channels reset and flags cleared, as the platform does after an error.
                unsafe { dma::quiesce() };
                return Err(f);
            }
        }
        for _ in 0..32 {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
        }
        n += 32;
        spins += 1;
        if spins > 20_000_000 {
            // SAFETY: as above.
            unsafe { dma::quiesce() };
            return Err(Poll::Busy);
        }
    }
    black_box(x);
    *steps += n;
    Ok(setup)
}

/// ALU steps and ticks of the same loop shape with the channel idle (what a free CPU does in that time).
fn idle_rate() -> (u64, u64) {
    let (mut x, mut n) = (0x1234_5678u32, 0u64);
    let t0 = ticks();
    for _ in 0..20_000 {
        for _ in 0..32 {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
        }
        n += 32;
        black_box(poll());
    }
    black_box(x);
    (n, (ticks() - t0).max(1))
}

/// One endpoint of a transfer: a fixed internal buffer, or a window in PSRAM that advances by one buffer per transfer.
#[derive(Clone, Copy)]
enum End {
    Sram(usize),
    Psram(usize),
}
impl End {
    fn at(self, i: usize) -> usize {
        match self {
            End::Sram(a) => a,
            End::Psram(a) => a + i * BUF,
        }
    }
    /// Contract of the platform's `dma.rs`: nothing dirty, nothing stale, before the engine touches it.
    fn prepare(self, count: usize) {
        match self {
            End::Sram(a) => wb_inv(a, BUF),
            End::Psram(a) => wb_inv(a, count * BUF),
        }
    }
}

fn case(name: &str, metric: &str, src: End, dst: End, burst: u32) -> bool {
    const COUNT: usize = (4 << 20) / BUF;
    let idle = idle_rate();
    src.prepare(COUNT);
    dst.prepare(COUNT);
    let (mut steps, mut setup_sum) = (0u64, 0u64);
    let t0 = ticks();
    for i in 0..COUNT {
        match xfer(src.at(i), dst.at(i), BUF, &mut steps) {
            Ok(c) => setup_sum += c as u64,
            Err(f) => {
                check("648.dma", Verdict::Fail, format_args!("{name}: transfer {i} failed: {f:?}"));
                return false;
            }
        }
    }
    let t = (ticks() - t0).max(1);
    // ALU steps per tick during the transfers against the same loop on an idle channel
    let free = pct_x100(steps * idle.1, t * idle.0);
    perf(
        metric,
        &[
            ("mb_s", Val::F2(mb_s_x100((COUNT * BUF) as u64, t, TICK_HZ) as i64)),
            ("cpu_free_pct", Val::F2(free as i64)),
            ("setup_cycles", Val::U(setup_sum / COUNT as u64)),
            ("burst_b", Val::U(burst as u64)),
        ],
    );
    true
}

/// Throughput of the four paths and what the CPU can do meanwhile. Returns false on a DMA fault.
pub fn run(burst: u32) -> bool {
    crate::con::begin("dma");
    let d = cached(PSRAM_OFFSETS.dma);
    let (a, b) = (buf_a(), buf_b());
    let paths = [
        ("sram->sram", "lab.dma.sram_to_sram", End::Sram(a), End::Sram(b)),
        ("sram->psram", "lab.dma.sram_to_psram", End::Sram(a), End::Psram(d)),
        ("psram->sram", "lab.dma.psram_to_sram", End::Psram(d), End::Sram(b)),
        ("psram->psram", "lab.dma.psram_to_psram", End::Psram(d), End::Psram(d + (4 << 20))),
    ];
    for (name, metric, src, dst) in paths {
        if !case(name, metric, src, dst, burst) {
            return false;
        }
    }
    true
}
