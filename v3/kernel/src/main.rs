//! Lichen v3 feasibility kernel (#607). Single hart, M-mode kernel, apps in U-mode behind PMP.
//! Not an OS yet: it exists to prove or disprove the isolation model on real silicon.
#![no_std]
#![no_main]

mod console;
mod csr;
mod global;
mod handles;
mod loader;
mod platform;
mod pmp;
mod sched;
mod syscall;
mod tests;
mod trap;

core::arch::global_asm!(include_str!("boot.S"));
core::arch::global_asm!(include_str!("trap.S"));

/// A kernel word that app tests try to read and overwrite; the harness checks it afterwards.
#[no_mangle]
pub static mut KCANARY: u32 = 0x5AFE_C0DE;
pub const KCANARY_VALUE: u32 = 0x5AFE_C0DE;

#[no_mangle]
extern "C" fn kmain() -> ! {
    platform::init();
    kprintln!("\nlichen-v3 poc kernel, platform {}, abi {}", platform::NAME, lichen_abi::ABI_VERSION);
    pmp::report();
    // SAFETY: called once, before any app runs and before interrupts are enabled; no other &mut Kernel exists.
    let k = unsafe { sched::KERNEL.get() };
    tests::boot(k)
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    kprintln!("KERNEL PANIC: {info}");
    platform::exit(2)
}
