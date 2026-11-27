//! App B: must keep running while app A misbehaves. It checks its own memory (a canary block) and its handle every
//! round; any corruption or a revoked handle ends it with a non-zero code, which the kernel's harness reports.
#![no_std]
#![no_main]

use core::fmt::Write;
use lichen_abi as abi;

const WORDS: usize = 64;
static mut CANARY: [u32; WORDS] = [0; WORDS];

fn pattern(i: usize) -> u32 {
    (i as u32).wrapping_mul(0x0101_0101) ^ 0xA5A5_A5A5
}

#[no_mangle]
pub extern "C" fn app_main(_a0: usize, _a1: usize, _a2: usize, _a3: usize) -> isize {
    let canary = core::ptr::addr_of_mut!(CANARY) as *mut u32;
    for i in 0..WORDS {
        // SAFETY: i < WORDS, the block is this app's own .bss and nothing else in the app aliases it.
        unsafe { canary.add(i).write_volatile(pattern(i)) };
    }
    let h = abi::handle_open(1);
    if abi::is_err(h) {
        return 4;
    }
    abi::write("B: up, canary set, handle open\n");
    let mut n: u32 = 0;
    loop {
        n = n.wrapping_add(1);
        for i in 0..WORDS {
            // SAFETY: as above.
            if unsafe { canary.add(i).read_volatile() } != pattern(i) {
                return 2;
            }
        }
        if abi::handle_use(h as usize) != 0 {
            return 3;
        }
        if n % 20000 == 0 {
            let mut l = abi::Line::new();
            let _ = writeln!(l, "B: alive, round {n}");
            abi::write(l.as_str());
        }
        abi::yield_now();
    }
}
