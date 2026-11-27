//! Probe hardware layer: UART0 output, trap bookkeeping, guarded M-mode accesses, PMP access, the M -> U harness.
//! One hart at a time, interrupts off in M: the statics are only touched by the hart running the tests (core 0's pmain,
//! or core 1's p1main while core 0 only watches the stage word) and by its trap_rust, never concurrently.

use core::arch::asm;
use core::fmt::{self, Write};
use core::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};
use core::sync::atomic::{compiler_fence, Ordering::SeqCst};

const UART0: usize = 0x500C_A000; // DR_REG_UART0_BASE (hw_ver1 reg_base.h:91)
const UART_STATUS: usize = UART0 + 0x1C; // TXFIFO_CNT in [23:16]

fn txfifo_count() -> u32 {
    // SAFETY: UART0 status register, a read without side effects.
    (unsafe { read_volatile(UART_STATUS as *const u32) } >> 16) & 0xFF
}
fn putc(c: u8) {
    while txfifo_count() >= 120 {}
    // SAFETY: UART0 TX FIFO; the ROM configured UART0 (115200 8N1) for download mode.
    unsafe { write_volatile(UART0 as *mut u32, c as u32) };
}
/// Wait until the TX FIFO is empty and the last byte has left the shift register.
pub fn drain() {
    while txfifo_count() != 0 {}
    spin(20_000);
}
pub fn spin(n: u32) {
    for i in 0..n {
        // SAFETY: a no-op; the asm keeps the loop from being optimised away.
        unsafe { asm!("nop", in("t0") i, options(nomem, nostack)) };
    }
}
pub struct Out;
impl Write for Out {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            if b == b'\n' {
                putc(b'\r');
            }
            putc(b);
        }
        Ok(())
    }
}
#[macro_export]
macro_rules! pl {
    ($($t:tt)*) => {{ let _ = core::fmt::Write::write_fmt(&mut $crate::hw::Out, format_args!($($t)*)); let _ = core::fmt::Write::write_str(&mut $crate::hw::Out, "\n"); }};
}

/// What a trap left behind: the raw CSRs plus a0/a1 of the trapped context (the U stubs return results there).
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Trap {
    pub cause: usize,
    pub epc: usize,
    pub tval: usize,
    pub mstatus: usize,
    pub a0: usize,
    pub a1: usize,
}
impl Trap {
    pub fn code(&self) -> usize {
        self.cause & 0xFFF
    }
    pub fn is_ecall_u(&self) -> bool {
        self.cause >> 31 == 0 && self.code() == 8
    }
}
impl fmt::Display for Trap {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let name = match (self.cause >> 31, self.code()) {
            (1, _) => "interrupt",
            (_, 1) => "instr-access-fault",
            (_, 2) => "illegal-instr",
            (_, 5) => "load-access-fault",
            (_, 7) => "store-access-fault",
            (_, 8) => "ecall-U",
            (_, 11) => "ecall-M",
            _ => "other",
        };
        write!(f, "{name} mcause={:#010x} mepc={:#010x} mtval={:#010x}", self.cause, self.epc, self.tval)
    }
}

static mut GUARD: bool = false;
static mut MTRAP_PRINTS: u32 = 0;
static mut MFAULT: Option<Trap> = None;
static mut UTRAP: Trap = Trap { cause: 0, epc: 0, tval: 0, mstatus: 0, a0: 0, a1: 0 };
pub static mut IRQ_COUNT: u32 = 0;
pub static mut IRQ_LAST: Trap = Trap { cause: 0, epc: 0, tval: 0, mstatus: 0, a0: 0, a1: 0 };
/// Called on every interrupt, before mret: must silence the source so it does not fire again.
pub static mut IRQ_QUIESCE: fn() = || {};

macro_rules! csr_read {
    ($n:literal) => {{
        let v: usize;
        // SAFETY: a CSR read in M-mode (trap handler or guarded call); it has no side effects.
        unsafe { core::arch::asm!(concat!("csrr {0}, ", $n), out(reg) v, options(nomem, nostack)) };
        v
    }};
}

#[no_mangle]
extern "C" fn trap_rust(frame: *mut [usize; 32]) -> usize {
    // SAFETY: frame points at the 128-byte register save area trap_entry just filled.
    let regs = unsafe { &*frame };
    let t = Trap {
        cause: csr_read!("mcause"),
        epc: csr_read!("mepc"),
        tval: csr_read!("mtval"),
        mstatus: csr_read!("mstatus"),
        a0: regs[10],
        a1: regs[11],
    };
    // SAFETY: single hart, M interrupts off: nothing else touches these statics while the handler runs.
    unsafe {
        if t.cause >> 31 != 0 {
            write_volatile(addr_of_mut!(IRQ_COUNT), read_volatile(addr_of!(IRQ_COUNT)) + 1);
            write_volatile(addr_of_mut!(IRQ_LAST), t);
            (read_volatile(addr_of!(IRQ_QUIESCE)))();
            return 0;
        }
        if (t.mstatus >> 11) & 3 == 3 {
            if read_volatile(addr_of!(GUARD)) {
                let n = read_volatile(addr_of!(MTRAP_PRINTS));
                if n < 8 {
                    write_volatile(addr_of_mut!(MTRAP_PRINTS), n + 1);
                    pl!("MTRAP {t}");
                    drain();
                }
                write_volatile(addr_of_mut!(MFAULT), Some(t));
                asm!("csrw mepc, {0}", in(reg) t.epc + 4, options(nomem, nostack));
                return 0;
            }
            pl!("FATAL unguarded M trap: {t}");
            pl!("END fatal");
            drain();
            loop {
                asm!("wfi", options(nomem, nostack));
            }
        }
        write_volatile(addr_of_mut!(UTRAP), t);
    }
    1
}

/// Run f with the M fault guard armed: a trap inside f is recorded and the faulting (4-byte) instruction skipped.
pub fn guarded(f: impl FnOnce() -> usize) -> Result<usize, Trap> {
    // SAFETY: see trap_rust; the fences keep the guard around the access.
    unsafe {
        write_volatile(addr_of_mut!(MFAULT), None);
        write_volatile(addr_of_mut!(GUARD), true);
    }
    compiler_fence(SeqCst);
    let v = f();
    compiler_fence(SeqCst);
    // SAFETY: as above.
    unsafe {
        write_volatile(addr_of_mut!(GUARD), false);
        match read_volatile(addr_of!(MFAULT)) {
            Some(t) => Err(t),
            None => Ok(v),
        }
    }
}
macro_rules! csr_try {
    ($n:literal) => {
        $crate::hw::guarded(|| {
            let v: usize;
            // SAFETY: guarded: an unimplemented CSR traps and is skipped (4-byte encoding forced).
            unsafe {
                core::arch::asm!(".option push", ".option norvc", concat!("csrr {0}, ", $n), ".option pop",
                    out(reg) v, options(nostack))
            };
            v
        })
    };
}

pub fn m_lw(a: usize) -> Result<u32, Trap> {
    guarded(|| {
        let v: usize;
        // SAFETY: guarded probe load; a fault is recorded and skipped.
        unsafe { asm!(".option push", ".option norvc", "lw {v}, 0({a})", ".option pop", v = out(reg) v, a = in(reg) a, options(nostack)) };
        v
    })
    .map(|v| v as u32)
}
pub fn m_sw(a: usize, v: u32) -> Result<(), Trap> {
    guarded(|| {
        // SAFETY: guarded probe store to a register or scratch address chosen by the test.
        unsafe { asm!(".option push", ".option norvc", "sw {v}, 0({a})", ".option pop", v = in(reg) v, a = in(reg) a, options(nostack)) };
        0
    })
    .map(|_| ())
}
pub fn m_lbu(a: usize) -> Result<u8, Trap> {
    guarded(|| {
        let v: usize;
        // SAFETY: guarded probe byte load.
        unsafe { asm!(".option push", ".option norvc", "lbu {v}, 0({a})", ".option pop", v = out(reg) v, a = in(reg) a, options(nostack)) };
        v
    })
    .map(|v| v as u8)
}
pub fn m_sb(a: usize, v: u8) -> Result<(), Trap> {
    guarded(|| {
        // SAFETY: guarded probe byte store.
        unsafe { asm!(".option push", ".option norvc", "sb {v}, 0({a})", ".option pop", v = in(reg) v, a = in(reg) a, options(nostack)) };
        0
    })
    .map(|_| ())
}

// PMP: CSR numbers must be immediates, so each index gets its own instruction.
macro_rules! csr_by_index {
    ($rname:ident, $wname:ident, [$($i:literal => $n:literal),*]) => {
        pub fn $rname(i: usize) -> usize {
            match i { $($i => csr_read!($n),)* _ => 0 }
        }
        pub fn $wname(i: usize, v: usize) {
            // SAFETY: M-mode PMP/PMA CSR write by the probe; locked entries ignore it (that is under test).
            match i { $($i => unsafe { asm!(concat!("csrw ", $n, ", {0}"), in(reg) v, options(nomem, nostack)) },)* _ => {} }
        }
    };
}
csr_by_index!(pmpcfg_r, pmpcfg_w, [0 => "0x3a0", 1 => "0x3a1", 2 => "0x3a2", 3 => "0x3a3"]);
csr_by_index!(pmpaddr_r, pmpaddr_w, [0 => "0x3b0", 1 => "0x3b1", 2 => "0x3b2", 3 => "0x3b3", 4 => "0x3b4",
    5 => "0x3b5", 6 => "0x3b6", 7 => "0x3b7", 8 => "0x3b8", 9 => "0x3b9", 10 => "0x3ba", 11 => "0x3bb",
    12 => "0x3bc", 13 => "0x3bd", 14 => "0x3be", 15 => "0x3bf"]);

pub const R: u8 = 1;
pub const W: u8 = 2;
pub const X: u8 = 4;
pub const TOR: u8 = 0x08;
pub const NAPOT: u8 = 0x18;
pub const L: u8 = 0x80;

pub fn pmp_cfg_byte(i: usize) -> u8 {
    (pmpcfg_r(i / 4) >> (8 * (i % 4))) as u8
}
pub fn pmp_cfg_set(i: usize, cfg: u8) {
    let sh = 8 * (i % 4);
    let v = (pmpcfg_r(i / 4) & !(0xFF << sh)) | ((cfg as usize) << sh);
    pmpcfg_w(i / 4, v);
}
pub fn napot(base: usize, size: usize) -> usize {
    (base >> 2) | ((size >> 3) - 1)
}
/// Program entry i (address first, then cfg), then fence so the next access sees it.
pub fn pmp_set(i: usize, addr: usize, cfg: u8) {
    pmp_cfg_set(i, 0);
    pmpaddr_w(i, addr);
    pmp_cfg_set(i, cfg);
    // SAFETY: orders the PMP update before later accesses.
    unsafe { asm!("fence", "fence.i", options(nostack)) };
}
pub fn pmp_off(i: usize) {
    pmp_cfg_set(i, 0);
}

extern "C" {
    fn run_user(entry: usize, usp: usize, a0: usize, a1: usize, mpie: usize);
    static _ustack_top: u8;
}
/// mret into U at entry (a U stub) with a0/a1; returns the exception that ended the run.
pub fn user(entry: usize, a0: usize, a1: usize, mpie: bool) -> Trap {
    // SAFETY: the harness saves and restores the callee-saved registers; UTRAP is written by trap_rust before return.
    unsafe {
        write_volatile(addr_of_mut!(UTRAP), Trap::default());
        run_user(entry, addr_of!(_ustack_top) as usize, a0, a1, mpie as usize);
        read_volatile(addr_of!(UTRAP))
    }
}
