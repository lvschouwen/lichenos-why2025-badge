//! Access-shape rows for the CPU-subsystem window (#646, review H1): the evidence so far covered `lw` and `sw` only.
//! One row per shape: byte and half loads, the compressed load, misaligned words (also straddling the window edges,
//! where one half is PMP-denied and a PMP-denied U load there hangs the P4), LR, AMOs, a PIE vector load with PIE
//! off and a float load with FS off. Each must TRAP (no hang, no value returned). Run last, each with a BEGIN line.
use crate::platform;
use crate::{kcheck, kprintln, mtrap};
use lichen_kcore::check::{Tally, Verdict};
use lichen_kcore::trap::{decode, exception_name};

core::arch::global_asm!(
    r#"
    .section .utext, "ax"
    .balign 4
    .global bs_lb
bs_lb:
    lb a0, 0(a0)
    li a7, 1
    ecall
    .global bs_lbu
bs_lbu:
    lbu a0, 0(a0)
    li a7, 1
    ecall
    .global bs_lh
bs_lh:
    lh a0, 0(a0)
    li a7, 1
    ecall
    .global bs_lhu
bs_lhu:
    lhu a0, 0(a0)
    li a7, 1
    ecall
    .global bs_clw
    .option push
    .option rvc
bs_clw:
    mv a1, a0
    c.lw a0, 0(a1)
    li a7, 1
    ecall
    .option pop
    .balign 4
    .global bs_lw
bs_lw:
    lw a0, 0(a0)
    li a7, 1
    ecall
    .global bs_lrw
bs_lrw:
    lr.w a0, (a0)
    li a7, 1
    ecall
    .global bs_amoadd
bs_amoadd:
    li t0, 1
    amoadd.w a0, t0, (a0)
    li a7, 1
    ecall
    .global bs_amoswap
bs_amoswap:
    li t0, 1
    amoswap.w a0, t0, (a0)
    li a7, 1
    ecall
# A PIE vector load, for the PIE-off illegal-instruction row. Espressif binutils 2.45 (esp-15.2.0_20251204), `.insn 4,
# 0x0201223b` disassembled (review L-d): objdump-xespv2p1 gives `esp.vld.128.ip q0,a0,16`; the default objdump and
# objdump-xespv2p2 give `esp.fft.ams.s16.ld.r32.decp q0,a0,q4,q0,q0,q2,q0,0,trunc` (v2p2 encodes esp.vld.128.ip
# q0,a0,16 as 0x0311009f). The IDF gdbstub decoder table (esp.vld.128.ip = 0x4200203b) uses the v2p1 layout, and
# either way it is a PIE instruction, which is what the row needs: with PIE off it must be illegal.
    .global bs_vld
bs_vld:
    .word 0x0201223b
    li a7, 1
    ecall
# flw fa0, 0(a0) (F is not in the target ISA string; encoded by hand)
    .global bs_flw
bs_flw:
    .word 0x00052507
    li a7, 1
    ecall
"#
);

extern "C" {
    fn bs_lb();
    fn bs_lbu();
    fn bs_lh();
    fn bs_lhu();
    fn bs_clw();
    fn bs_lw();
    fn bs_lrw();
    fn bs_amoadd();
    fn bs_amoswap();
    fn bs_vld();
    fn bs_flw();
}

/// Exception codes: 2 illegal, 4 load misaligned, 5 load fault, 6 store/AMO misaligned, 7 store/AMO fault.
const fn codes(list: &[u16]) -> u32 {
    let mut m = 0;
    let mut i = 0;
    while i < list.len() {
        m |= 1 << list[i];
        i += 1;
    }
    m
}

/// One access shape; core 1 runs the same table (bringup_core1, review H1 on both harts).
pub struct Shape {
    pub id: &'static str,
    pub f: unsafe extern "C" fn(),
    pub addr: usize,
    /// Accepted exception codes as a bit mask (bit n = code n).
    pub want: u32,
}

const W: usize = 0x2000_4000;
const LOAD: u32 = codes(&[5]);
const AMO: u32 = codes(&[5, 7]);
const MISALIGNED: u32 = codes(&[4, 5]);
const ILLEGAL: u32 = codes(&[2]);

/// Safe shapes first, the ones with a PMP-denied half last.
pub const SHAPES: [Shape; 16] = [
    Shape { id: "u.shape.lb", f: bs_lb, addr: W, want: LOAD },
    Shape { id: "u.shape.lbu", f: bs_lbu, addr: W + 1, want: LOAD },
    Shape { id: "u.shape.lh", f: bs_lh, addr: W, want: LOAD },
    Shape { id: "u.shape.lhu", f: bs_lhu, addr: W + 2, want: LOAD },
    Shape { id: "u.shape.c.lw", f: bs_clw, addr: W, want: LOAD },
    Shape { id: "u.shape.lr.w", f: bs_lrw, addr: W, want: LOAD },
    Shape { id: "u.shape.amoadd.w", f: bs_amoadd, addr: W, want: AMO },
    Shape { id: "u.shape.amoswap.w", f: bs_amoswap, addr: W, want: AMO },
    Shape { id: "u.shape.pie-vld", f: bs_vld, addr: W, want: ILLEGAL },
    Shape { id: "u.shape.flw-fs-off", f: bs_flw, addr: W, want: ILLEGAL },
    Shape { id: "u.shape.lw-misaligned", f: bs_lw, addr: W + 2, want: MISALIGNED },
    Shape { id: "u.shape.lh-misaligned", f: bs_lh, addr: W + 1, want: MISALIGNED },
    Shape { id: "u.shape.lw-straddle-low", f: bs_lw, addr: 0x1FFF_FFFE, want: MISALIGNED },
    Shape { id: "u.shape.lw-straddle-high", f: bs_lw, addr: 0x2FFF_FFFE, want: MISALIGNED },
    Shape { id: "u.shape.lh-straddle-low", f: bs_lh, addr: 0x1FFF_FFFF, want: MISALIGNED },
    // Review L-e: the half straddling the window's top edge (0x2FFF_FFFF in the window, 0x3000_0000 PMP-denied).
    Shape { id: "u.shape.lh-straddle-high", f: bs_lh, addr: 0x2FFF_FFFF, want: MISALIGNED },
];

pub fn run(t: &mut Tally, sp: usize) {
    for s in &SHAPES {
        kprintln!("BEGIN {} {:#010x}", s.id, s.addr);
        platform::flush();
        let u = mtrap::run_u(s.f as usize, sp, s.addr, 0);
        let fault = u.fault.map(|c| (decode(c.cause).code, c.tval, c.epc));
        let ok = fault.is_some_and(|(code, _, _)| s.want & (1 << code) != 0) && u.exit.is_none();
        let (code, tval, epc) = fault.unwrap_or((0xFFF, 0, 0));
        let v = if ok { Verdict::Pass } else { Verdict::Fail };
        t.record(v);
        kcheck!(
            s.id,
            v,
            "U access at {:#010x}: code {code} ({}) mtval {tval:#x} mepc {epc:#x}, exit {:x?} (want code mask {:#x})",
            s.addr,
            exception_name(code),
            u.exit,
            s.want
        );
        if u.exit.is_some() {
            kprintln!("FINDING {}: the access RETURNED {:x?} instead of trapping", s.id, u.exit);
        }
    }
}
