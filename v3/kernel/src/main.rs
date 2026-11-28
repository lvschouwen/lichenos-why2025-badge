//! Lichen v3 kernel. Two builds share this crate:
//! - `qemu-virt`: the #607 feasibility harness (single hart, M-mode kernel, apps in U-mode behind PMP), 21 isolation
//!   cases on an emulator. Proves kernel logic only.
//! - `esp32p4`: the RAM-only bring-up image for the real chip (#611): banner, bench CHECK lines, END. Loaded by the
//!   ROM download mode, never written to flash. M-mode traps, the tick, PMP and core 1 are #612; the run ends with
//!   the same 21 isolation cases as qemu-virt, apps in NOLOAD L2MEM slots (#613).
#![no_std]
#![no_main]

#[cfg(all(feature = "qemu-virt", feature = "esp32p4"))]
compile_error!("enable exactly one platform feature: qemu-virt or esp32p4");

mod console;
mod global;
mod platform;

#[cfg(feature = "esp32p4")]
mod bringup;
#[cfg(feature = "esp32p4")]
mod bringup_core1;
#[cfg(feature = "esp32p4")]
mod bringup_pmp;
#[cfg(feature = "esp32p4")]
mod bringup_shapes;
#[cfg(feature = "esp32p4")]
mod bringup_u;
#[cfg(feature = "esp32p4")]
mod mtrap;
#[cfg(feature = "esp32p4")]
mod smp;
#[cfg(feature = "esp32p4")]
mod timebase;

// The isolation harness (#607): the same scheduler, loader, syscalls and cases on both platforms (#613).
mod csr;
mod loader;
mod pmp;
mod sched;
mod syscall;
mod tests;
mod trap;
use lichen_kcore::handles;

#[cfg(feature = "esp32p4")]
mod bringup_iso;

#[cfg(feature = "qemu-virt")]
core::arch::global_asm!(include_str!("boot.S"));
core::arch::global_asm!(include_str!("trap.S"));
#[cfg(feature = "esp32p4")]
core::arch::global_asm!(include_str!("boot_esp32p4.S"));

/// A kernel word that app tests try to read and overwrite; the harness checks it afterwards.
#[no_mangle]
pub static mut KCANARY: u32 = 0x5AFE_C0DE;
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

/// Entry contract (same for the stage-booted and the load-ram route): M-mode, interrupts off, a0 = hart id, a1 = pointer
/// to the boot-info struct (#615 owns it) or NULL under load-ram. The struct is not interpreted here.
#[cfg(feature = "esp32p4")]
#[no_mangle]
extern "C" fn kmain(hart: usize, boot_info: *const u8) -> ! {
    bringup::run(hart, boot_info)
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    // The panicking code may hold the console ring, or run on core 1 while core 0 prints: go raw first.
    #[cfg(feature = "esp32p4")]
    platform::enter_fatal();
    kprintln!("KERNEL PANIC: {info}");
    platform::exit(2)
}
