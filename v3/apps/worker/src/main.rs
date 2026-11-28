//! The well-behaved process of the #618 stage: it counts rounds, checks its own memory (a canary block) and its
//! handle every round, and never yields or exits, so only the tick moves it aside and only its budget (none here) or
//! the stage's deadline ends it. Corruption or a revoked handle ends it with a non-zero code. It prints nothing, unless
//! a0 makes it a pinger too (#619: a short WRITE every a0 rounds, forwarded to core 0's console).
#![no_std]
#![no_main]

use lichen_abi as abi;

/// The round counter is the first word of the data region (`.data.first`, apps/app.ld.in), where core 0 can read it
/// after the stage without knowing this app's layout.
#[link_section = ".data.first"]
static mut ROUNDS: u32 = 0;
const WORDS: usize = 32;
static mut CANARY: [u32; WORDS] = [0; WORDS];

fn pattern(i: usize) -> u32 {
    (i as u32).wrapping_mul(0x0101_0101) ^ 0xC3C3_C3C3
}

#[no_mangle]
pub extern "C" fn app_main(a0: usize, _a1: usize, _a2: usize, _a3: usize) -> isize {
    // a0 != 0 (#619): also a pinger, a short forwarded WRITE every a0 rounds; one that comes back short ends it (7).
    let ping_every = a0 as u32;
    let canary = core::ptr::addr_of_mut!(CANARY) as *mut u32;
    let rounds = core::ptr::addr_of_mut!(ROUNDS);
    for i in 0..WORDS {
        // SAFETY: i < WORDS; the block is this app's own .bss and nothing else in the app aliases it.
        unsafe { canary.add(i).write_volatile(pattern(i)) };
    }
    let h = abi::handle_open(1);
    if abi::is_err(h) {
        return 4;
    }
    loop {
        // SAFETY: ROUNDS is this app's own word, written only here.
        unsafe { rounds.write_volatile(rounds.read_volatile().wrapping_add(1)) };
        for i in 0..WORDS {
            // SAFETY: as above.
            if unsafe { canary.add(i).read_volatile() } != pattern(i) {
                return 2;
            }
        }
        if abi::handle_use(h as usize) != 0 {
            return 3;
        }
        // SAFETY: as above.
        if ping_every != 0 && unsafe { rounds.read_volatile() }.is_multiple_of(ping_every) && abi::write("w\n") != 2 {
            return 7;
        }
    }
}
