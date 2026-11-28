//! Lichen v3 PoC application ABI (version 1).
//!
//! The ABI is registers and numbers only, never Rust layouts: `ecall` with the syscall number in a7, arguments in
//! a0..a2, the result in a0. A result in [-4095, -1] is an error code; anything else is a value. That keeps it callable
//! from C and C++ apps unchanged.
#![no_std]

pub const ABI_VERSION: u32 = 1;
/// "LAPP", little endian: the first word of every app image.
pub const IMAGE_MAGIC: u32 = 0x5050_414C;

pub mod sys {
    pub const EXIT: usize = 0;
    pub const WRITE: usize = 1;
    pub const YIELD: usize = 2;
    pub const TICKS: usize = 3;
    pub const SBRK: usize = 4;
    pub const HANDLE_OPEN: usize = 5;
    pub const HANDLE_USE: usize = 6;
    pub const HANDLE_CLOSE: usize = 7;
}

pub mod err {
    pub const EPERM: isize = -1;
    pub const EBADF: isize = -9;
    pub const EAGAIN: isize = -11;
    pub const ENOMEM: isize = -12;
    pub const EFAULT: isize = -14;
    pub const EINVAL: isize = -22;
    pub const EMFILE: isize = -24;
    pub const ENOSYS: isize = -38;
}

/// Longest WRITE the kernel accepts in one call.
pub const WRITE_MAX: usize = 256;

pub fn is_err(r: isize) -> bool {
    (-4095..0).contains(&r)
}

#[cfg(target_arch = "riscv32")]
#[inline(always)]
pub fn syscall(n: usize, a0: usize, a1: usize, a2: usize) -> isize {
    let r: isize;
    // SAFETY: ecall traps to the kernel, which validates every argument and only writes a0 back. No memory is
    // touched on the app side, so no Rust invariant is affected.
    unsafe {
        core::arch::asm!("ecall", inlateout("a0") a0 => r, in("a1") a1, in("a2") a2, in("a7") n,
                         options(nostack));
    }
    r
}

#[cfg(not(target_arch = "riscv32"))]
pub fn syscall(_n: usize, _a0: usize, _a1: usize, _a2: usize) -> isize {
    err::ENOSYS
}

// The kernel ends the task on EXIT, so the loop is never reached; an app that survives has nowhere to go.
#[allow(clippy::empty_loop)]
pub fn exit(code: isize) -> ! {
    syscall(sys::EXIT, code as usize, 0, 0);
    loop {}
}
pub fn write(s: &str) -> isize {
    syscall(sys::WRITE, s.as_ptr() as usize, s.len(), 0)
}
pub fn yield_now() {
    syscall(sys::YIELD, 0, 0, 0);
}
pub fn sbrk(incr: isize) -> isize {
    syscall(sys::SBRK, incr as usize, 0, 0)
}
pub fn handle_open(kind: usize) -> isize {
    syscall(sys::HANDLE_OPEN, kind, 0, 0)
}
pub fn handle_use(h: usize) -> isize {
    syscall(sys::HANDLE_USE, h, 0, 0)
}
pub fn handle_close(h: usize) -> isize {
    syscall(sys::HANDLE_CLOSE, h, 0, 0)
}

/// A fixed-size line buffer so apps can format without an allocator.
pub struct Line {
    buf: [u8; 128],
    len: usize,
}
impl Line {
    pub const fn new() -> Self {
        Self { buf: [0; 128], len: 0 }
    }
    pub fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.len]).unwrap_or("?")
    }
}
impl Default for Line {
    fn default() -> Self {
        Self::new()
    }
}
impl core::fmt::Write for Line {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let n = s.len().min(self.buf.len() - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

#[cfg(all(feature = "rt", target_arch = "riscv32"))]
mod rt {
    // The kernel enters with a0..a3 = arguments and sp = top of the app's data region. _start copies .data from its
    // load address in the image, zeroes .bss (t registers only, so a0..a3 survive) and calls app_main.
    core::arch::global_asm!(
        ".section .text.start, \"ax\"",
        ".global _start",
        "_start:",
        "  la t0, _sidata",
        "  la t1, _sdata",
        "  la t2, _edata",
        "1: bgeu t1, t2, 2f",
        "  lw t3, 0(t0)",
        "  sw t3, 0(t1)",
        "  addi t0, t0, 4",
        "  addi t1, t1, 4",
        "  j 1b",
        "2: la t1, _sbss",
        "  la t2, _ebss",
        "3: bgeu t1, t2, 4f",
        "  sw zero, 0(t1)",
        "  addi t1, t1, 4",
        "  j 3b",
        "4: call app_main",
        "  li a7, 0",
        "  ecall",
        "  unimp",
    );

    #[panic_handler]
    fn panic(_: &core::panic::PanicInfo) -> ! {
        super::exit(-99)
    }
}
