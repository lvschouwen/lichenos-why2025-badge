//! Lichen v3 kernel. Two builds share this crate:
//! - `qemu-virt`: the #607 feasibility harness (single hart, M-mode kernel, apps in U-mode behind PMP), 21 isolation
//!   cases on an emulator. Proves kernel logic only.
//! - `esp32p4`: the RAM-only bring-up image for the real chip (#611): banner, bench CHECK lines, END. Loaded by the
//!   ROM download mode, never written to flash. Traps, PMP and core 1 arrive with #612.
#![no_std]
#![no_main]

#[cfg(all(feature = "qemu-virt", feature = "esp32p4"))]
compile_error!("enable exactly one platform feature: qemu-virt or esp32p4");

mod console;
mod global;
mod platform;

#[cfg(feature = "esp32p4")]
mod bringup;

#[cfg(feature = "qemu-virt")]
use lichen_kcore::handles;
#[cfg(feature = "qemu-virt")]
mod csr;
#[cfg(feature = "qemu-virt")]
mod loader;
#[cfg(feature = "qemu-virt")]
mod pmp;
#[cfg(feature = "qemu-virt")]
mod sched;
#[cfg(feature = "qemu-virt")]
mod syscall;
#[cfg(feature = "qemu-virt")]
mod tests;
#[cfg(feature = "qemu-virt")]
mod trap;

#[cfg(feature = "qemu-virt")]
core::arch::global_asm!(include_str!("boot.S"));
#[cfg(feature = "qemu-virt")]
core::arch::global_asm!(include_str!("trap.S"));
#[cfg(feature = "esp32p4")]
core::arch::global_asm!(include_str!("boot_esp32p4.S"));

/// A kernel word that app tests try to read and overwrite; the harness checks it afterwards.
#[cfg(feature = "qemu-virt")]
#[no_mangle]
pub static mut KCANARY: u32 = 0x5AFE_C0DE;
#[cfg(feature = "qemu-virt")]
pub const KCANARY_VALUE: u32 = 0x5AFE_C0DE;

#[cfg(feature = "qemu-virt")]
#[no_mangle]
extern "C" fn kmain() -> ! {
    platform::init();
    kprintln!("\nlichen-v3 poc kernel, platform {}, abi {}", platform::NAME, lichen_abi::ABI_VERSION);
    pmp::report();
    // SAFETY: called once, before any app runs and before interrupts are enabled; no other &mut Kernel exists.
    let k = unsafe { sched::KERNEL.get() };
    tests::boot(k)
}

#[cfg(feature = "esp32p4")]
#[no_mangle]
extern "C" fn kmain() -> ! {
    bringup::run()
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    kprintln!("KERNEL PANIC: {info}");
    platform::exit(2)
}
