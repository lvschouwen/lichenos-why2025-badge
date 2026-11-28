//! The misbehaving process of the #618 stage; a0 picks how. It opens a handle first, so the kernel has something to
//! take back when it ends it. a1 = an address in the other app's data. Its WRITEs go to core 0's console (#619).
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
/// a0: a flood of full-size WRITEs for ever (#619). Each one is forwarded to core 0's console and blocks this process
/// until core 0 answers; the worker must keep running meanwhile. Exit 5: WRITE was refused (EPERM, not forwarded);
/// exit 6: a WRITE came back with less than it wrote.
const WRITE_LOOP: usize = 3;
/// a0: PINGS one-byte WRITEs, each checked (exit 6 if short), a newline, then exit 0: the round-trip latency run.
const PING: usize = 4;
const PINGS: u32 = 100;

/// WRITEs that came back complete, the first word of the data region (`.data.first`, apps/app.ld.in): core 0 reads it
/// after the stage.
#[link_section = ".data.first"]
static mut WRITES: u32 = 0;

/// One WRITE that must come back complete.
fn write_all(s: &str) -> Result<(), isize> {
    match abi::write(s) {
        r if r == s.len() as isize => {
            let w = core::ptr::addr_of_mut!(WRITES);
            // SAFETY: WRITES is this app's own word, written only here (single-threaded app).
            unsafe { w.write_volatile(w.read_volatile().wrapping_add(1)) };
            Ok(())
        }
        abi::err::EPERM => Err(5),
        _ => Err(6),
    }
}

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
        WRITE_LOOP => {
            let mut line = [b'x'; abi::WRITE_MAX];
            line[abi::WRITE_MAX - 1] = b'\n';
            // SAFETY: bytes of `line` are ASCII.
            let s = unsafe { core::str::from_utf8_unchecked(&line) };
            loop {
                if let Err(code) = write_all(s) {
                    return code;
                }
            }
        }
        PING => {
            for _ in 0..PINGS {
                if let Err(code) = write_all(".") {
                    return code;
                }
            }
            return write_all("\n").err().unwrap_or(0);
        }
        _ => return -1,
    }
    // The store did not fault: isolation failed.
    100
}
