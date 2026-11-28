//! The misbehaving process of the #618 stage; a0 picks how. It opens a handle first, so the kernel has something to
//! take back when it ends it. a1 = an address in the other app's data. Core 1's console is raw, so it prints nothing.
#![no_std]
#![no_main]

use lichen_abi as abi;

/// a0: store to the other app's memory (must fault; the kernel ends only this process).
const CRASH: usize = 0;
/// a0: spin for ever (never faults; only the cycle budget ends it).
const SPIN: usize = 1;
/// a0: exit with `EXIT_CODE` at once. Its image replaces another app's in the same slot (the ICache case): the code
/// that runs is this one only if the instruction fetch sees the new bytes.
const EXIT: usize = 2;
const EXIT_CODE: isize = 77;

#[no_mangle]
pub extern "C" fn app_main(mode: usize, other: usize, _a2: usize, _a3: usize) -> isize {
    if abi::is_err(abi::handle_open(1)) {
        return 4;
    }
    match mode {
        // SAFETY: the access is the point of the test; it is expected to trap and the kernel ends this process on it.
        CRASH => unsafe { core::ptr::write_volatile(other as *mut u32, 0xBAD0_BAD0) },
        SPIN => loop {
            core::hint::spin_loop();
        },
        EXIT => return EXIT_CODE,
        _ => return -1,
    }
    // The store did not fault: isolation failed.
    100
}
