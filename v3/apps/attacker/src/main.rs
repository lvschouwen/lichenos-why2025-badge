//! App A: one deliberate violation per test id (a0). a1 = the test's target (address or handle; for 4 a spot in this
//! app's own data, for 5 its own entry), a2 = app B's data base, a3 = a kernel data address. The kernel checks that
//! each fault points at a1 (tests.rs). A fault test that reaches its `return 100` means the protection did NOT
//! trigger. Cases 16..18 are ESP32-P4 extension state (the qemu-virt kernel never runs them).
#![no_std]
#![no_main]

use lichen_abi::{self as abi, err};

fn raw_write(ptr: usize, len: usize) -> isize {
    abi::syscall(abi::sys::WRITE, ptr, len, 0)
}

#[no_mangle]
pub extern "C" fn app_main(test: usize, a1: usize, a2: usize, a3: usize) -> isize {
    // SAFETY (all arms): these accesses are the point of the test; each one is expected to trap, and the kernel ends
    // this app on the trap. None of them aliases a Rust reference held by this app.
    unsafe {
        match test {
            0 => {
                core::ptr::read_volatile(a1 as *const u32);
            }
            1 => core::ptr::write_volatile(a1 as *mut u32, 0xBAD0_BAD0),
            2 => {
                core::ptr::read_volatile(a1 as *const u32);
            }
            3 => core::ptr::write_volatile(a1 as *mut u32, 0xBAD0_BAD0),
            4 => {
                // c.ret into our own (RW, not X) data at a1 (the kernel passes our heap start), then jump to it.
                let p = a1 as *mut u16;
                p.write_volatile(0x8082);
                let f: extern "C" fn() = core::mem::transmute(p);
                f();
            }
            5 => core::ptr::write_volatile(a1 as *mut u32, 0),
            6 => {
                core::ptr::read_volatile(a1 as *const u32);
            }
            7 => core::arch::asm!("csrw pmpcfg0, zero"),
            8 => core::arch::asm!("mret"),
            9 => core::arch::asm!("unimp"),
            10 => return syscall_args(a2, a3),
            11 => return handles(a1),
            12 => return memory(),
            13 => loop {
                core::hint::spin_loop();
            },
            14 => {
                let f: extern "C" fn() = core::mem::transmute(a1);
                f();
            }
            15 => core::arch::asm!("csrw mtvec, zero"),
            // fmv.w.x f0, zero: F is not in the target ISA string, so encoded by hand. mstatus.FS is off.
            16 => core::arch::asm!(".word 0xF0000053"),
            // esp.lp.setupi 0, 4, +8. HWLP state is off. Round trip with Espressif binutils esp-15.2.0_20251204:
            // `as -march=rv32imafc_xesploop` assembles `esp.lp.setupi 0, 4, 1f` to 0x0042502b, and objdump of that
            // object prints `0042502b  esp.lp.setupi 0,4,8`. (Without the xesploop attribute objdump shows `.word`.)
            17 => core::arch::asm!(".word 0x0042502b"),
            // A PIE vector load (bringup_shapes.rs `u.shape.pie-vld`, the same word). PIE state is off.
            18 => core::arch::asm!(".word 0x0201223b"),
            _ => return -1,
        }
    }
    100
}

fn syscall_args(other_app: usize, kernel: usize) -> isize {
    let ok = "ok";
    if raw_write(kernel, 4) != err::EFAULT {
        return 1;
    }
    if raw_write(other_app, 4) != err::EFAULT {
        return 2;
    }
    if raw_write(0xFFFF_FFF0, 0x20) != err::EFAULT {
        return 3;
    }
    if abi::syscall(99, 0, 0, 0) != err::ENOSYS {
        return 4;
    }
    if raw_write(ok.as_ptr() as usize, abi::WRITE_MAX + 1) != err::EINVAL {
        return 5;
    }
    if raw_write(0, 4) != err::EFAULT {
        return 6;
    }
    0
}

fn handles(stolen: usize) -> isize {
    if abi::handle_use(stolen) != err::EBADF {
        return 1;
    }
    let h = abi::handle_open(0);
    if abi::is_err(h) {
        return 2;
    }
    let h = h as usize;
    if abi::handle_use(h) != 0 {
        return 3;
    }
    if abi::handle_close(h) != 0 {
        return 4;
    }
    if abi::handle_use(h) != err::EBADF {
        return 5;
    }
    if abi::handle_close(h) != err::EBADF {
        return 6;
    }
    for idx in 0..16usize {
        for gen in 0..8usize {
            if abi::handle_use(gen << 8 | idx) != err::EBADF {
                return 7;
            }
        }
    }
    let mut opened = 0;
    loop {
        let r = abi::handle_open(0);
        if r == err::EMFILE {
            break;
        }
        if abi::is_err(r) || opened > 16 {
            return 8;
        }
        opened += 1;
    }
    if opened == 0 {
        return 9;
    }
    if abi::handle_use(usize::MAX) != err::EBADF {
        return 10;
    }
    0
}

fn memory() -> isize {
    let mut grants = 0;
    loop {
        let r = abi::sbrk(4096);
        if r == err::ENOMEM {
            break;
        }
        if abi::is_err(r) || grants > 64 {
            return 1;
        }
        // The grant must be usable.
        // SAFETY: the kernel just granted [r, r+4096) inside this app's own data region.
        unsafe { core::ptr::write_bytes(r as usize as *mut u8, 0x5A, 4096) };
        grants += 1;
    }
    if grants == 0 {
        return 2;
    }
    if abi::sbrk(-0x1000_0000) != err::EINVAL {
        return 3;
    }
    0
}
