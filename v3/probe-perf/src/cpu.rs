//! CPU-bound kernels with the code and the data in internal RAM and again in PSRAM. Code in PSRAM is written through the cache, written
//! back, and made visible to the instruction cache (the ROM invalidates its range, then `fence.i`): a kernel that is position
//! independent (`k_alu`, generated straight-line code) runs from wherever it is copied.
use crate::con::{check, perf};
use crate::hw::{mcycle, ticks, TICK_HZ};
use crate::mem::*;
use core::hint::black_box;
use lichen_kcore::check::Verdict;
use lichen_perflab::kern::{crc32_update, ratio_x100, straight_line};
use lichen_perflab::line::Val;
use lichen_perflab::rate::{mb_s_x100, mops_x100};

// a = iterations. Nine instructions per round: add, xor, slli, add, mul, srli, xor, addi, bnez. No memory, no call, no address.
core::arch::global_asm!(
    r#"
.option push
.option norvc
.section .text.kernels, "ax"
.balign 4
.global k_alu
k_alu:
    li t0, 0x12345678
    li t1, 0x9e3779b9
    li t2, 1
1:  add t0, t0, t1
    xor t2, t2, t0
    slli t3, t2, 5
    add t0, t0, t3
    mul t4, t0, t1
    srli t5, t4, 7
    xor t2, t2, t5
    addi a0, a0, -1
    bnez a0, 1b
    add a0, t0, t2
    ret
.global k_alu_end
k_alu_end:
.option pop
"#
);
extern "C" {
    fn k_alu(n: u32) -> u32;
    static k_alu_end: u8;
}
const ALU_INSTR: u64 = 9;

const ROM_CACHE_INVALIDATE_ADDR: usize = 0x4FC0_03E4;
const MAP_ICACHE: u32 = 0b11; // CACHE_MAP_L1_ICACHE_0 | _1

/// Make code just written at `addr` runnable: data cache written back, instruction cache dropped for the range, pipeline refetch.
fn make_code_visible(addr: usize, len: usize) {
    wb_inv(addr, len);
    // SAFETY: the ROM's Cache_Invalidate_Addr(map, addr, size), then fence.i; the range holds code the caller just wrote.
    unsafe {
        let f: extern "C" fn(u32, u32, u32) -> i32 = core::mem::transmute(ROM_CACHE_INVALIDATE_ADDR);
        f(MAP_ICACHE, addr as u32, len as u32);
        core::arch::asm!(".option push", ".option arch, +zifencei", "fence.i", ".option pop");
    }
}

/// Cycles (mcycle) and SYSTIMER ticks of `f`.
fn timed(f: impl FnOnce()) -> (u32, u64) {
    let (c0, t0) = (mcycle(), ticks());
    f();
    (mcycle().wrapping_sub(c0), ticks() - t0)
}

fn report_code(metric: &str, instrs: u64, (cycles, t): (u32, u64)) {
    perf(
        metric,
        &[
            ("cycles_per_instr", Val::F2(ratio_x100(cycles as u64, instrs) as i64)),
            ("minstr_s", Val::F2(mops_x100(instrs, t, TICK_HZ) as i64)),
        ],
    );
}

fn alu() {
    const N: u32 = 3_000_000;
    black_box(unsafe { k_alu(1000) }); // SAFETY: the kernel is plain computation, no memory, no stack.
    let m = timed(|| {
        // SAFETY: as above.
        black_box(unsafe { k_alu(N) });
    });
    report_code("lab.cpu.alu.code_sram", N as u64 * ALU_INSTR, m);
    // the same kernel, copied into PSRAM
    // the two symbols bound the kernel's code
    let start = k_alu as *const () as usize;
    let len = core::ptr::addr_of!(k_alu_end) as usize - start;
    let dst = cached(PSRAM_OFFSETS.chase);
    for i in (0..len).step_by(4) {
        // SAFETY: reads the kernel's own words.
        wr(dst + i, unsafe { core::ptr::read((start + i) as *const u32) });
    }
    make_code_visible(dst, len);
    // SAFETY: a copy of position-independent code (no branch leaves it, no address inside it).
    let f: extern "C" fn(u32) -> u32 = unsafe { core::mem::transmute(dst) };
    black_box(f(1000));
    let m = timed(|| {
        black_box(f(N));
    });
    report_code("lab.cpu.alu.code_psram", N as u64 * ALU_INSTR, m);
}

/// Straight-line code of `adds` dependent adds, called `calls` times: with more code than the L1 instruction cache (16 KB) every call
/// refetches it, from internal RAM, from L2 (24 KB in PSRAM) or from PSRAM itself (512 KB, more than the 256 KB L2).
fn fetch_case(metric: &str, addr: usize, adds: usize, calls: u32) {
    // SAFETY: `addr` is internal RAM or the mapped window with room for adds + 1 words (checked by the caller's choice of region).
    let code = unsafe { core::slice::from_raw_parts_mut(addr as *mut u32, adds + 1) };
    if straight_line(code, adds).is_none() {
        check("648.cpu.fetch", Verdict::Fail, format_args!("{metric}: code does not fit"));
        return;
    }
    make_code_visible(addr, (adds + 1) * 4);
    // SAFETY: the words are `add t0, t0, t1` and a `ret`: a leaf function that clobbers t0 only.
    let f: extern "C" fn() = unsafe { core::mem::transmute(addr) };
    f(); // the cold pass
    let m = timed(|| {
        for _ in 0..calls {
            f();
        }
    });
    report_code(metric, adds as u64 * calls as u64, m);
}

fn fetch() {
    let (a, b) = (buf_a(), buf_b());
    if b != a + BUF {
        check(
            "648.cpu.fetch",
            Verdict::Fail,
            format_args!("the two buffers are not adjacent: no 32 KB for the internal RAM code"),
        );
        return;
    }
    let p = cached(PSRAM_OFFSETS.chase);
    fetch_case("lab.cpu.fetch.sram24k", a, 6143, 4000);
    fetch_case("lab.cpu.fetch.psram24k", p, 6143, 4000);
    fetch_case("lab.cpu.fetch.psram512k", p + (1 << 20), (512 << 10) / 4 - 1, 190);
}

/// A CPU-bound loop over data: the same buffer in internal RAM and in PSRAM must give the same CRC, and the rate says what the cache
/// makes of the placement (16 KB sits in L1 after the first pass, so the two should agree).
fn crc() {
    const PASSES: u32 = 24;
    let (src, dst) = (buf_a(), cached(PSRAM_OFFSETS.read));
    // SAFETY: both ranges are 16 KB of mapped memory that nothing else uses now.
    let (s, d) = unsafe {
        (core::slice::from_raw_parts_mut(src as *mut u8, BUF), core::slice::from_raw_parts_mut(dst as *mut u8, BUF))
    };
    let mut x = 0x2545_F491u32;
    for b in s.iter_mut() {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *b = x as u8;
    }
    d.copy_from_slice(s);
    wb_inv(dst, BUF);
    let run = |buf: &[u8]| {
        let mut c = !0;
        for _ in 0..PASSES {
            c = crc32_update(c, buf);
        }
        c
    };
    let (mut crc_s, mut crc_d) = (0, 0);
    let ms = timed(|| crc_s = run(s));
    let md = timed(|| crc_d = run(d));
    for (metric, (_, t)) in [("lab.cpu.crc.data_sram", ms), ("lab.cpu.crc.data_psram", md)] {
        perf(metric, &[("mb_s", Val::F2(mb_s_x100(BUF as u64 * PASSES as u64, t, TICK_HZ) as i64))]);
    }
    let ok = crc_s == crc_d;
    crate::con::pass_if(ok, "648.cpu.crc", format_args!("same data, internal RAM {crc_s:#010x}, PSRAM {crc_d:#010x}"));
}

pub fn run() {
    crate::con::begin("cpu");
    alu();
    fetch();
    crc();
}
