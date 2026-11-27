//! Lichen v3 boot stage for the ESP32-P4 (#615), step 1: the RAM proof. Loaded with load-ram (or, one day, by the ROM
//! from flash 0x2000), it prints, switches off the ROM's watchdogs, reads the partition table, otadata and both app
//! slots from flash, SHA-256-verifies them and reports. It never jumps, never writes flash, never touches the eFuse
//! controller and does no MMU, cache, clock, PSRAM or C6-pin work.
#![no_std]
#![no_main]

mod hw;

use boot_core::console::Console;
use hw::{Uart, MEMORY_MAP};

core::arch::global_asm!(include_str!("boot.S"));

/// Entry contract: a0 = hart id, a1 = junk under load-ram (the ROM leaves 0x14) and not used by this stage.
#[no_mangle]
extern "C" fn stage_main(hart: u32, a1: u32) -> ! {
    let mut con = Uart;
    con.line("");
    con.line("LBOOT stage up");
    con.puts("a0 ");
    con.hex32(hart);
    con.puts(" a1 ");
    con.hex32(a1);
    con.put(b'\n');
    con.line("step: watchdogs");
    let w = hw::wdt_state();
    con.puts("wdt before: timg0 ");
    con.hex32(w[0]);
    con.puts(" timg1 ");
    con.hex32(w[1]);
    con.puts(" rwdt ");
    con.hex32(w[2]);
    con.puts(" swd ");
    con.hex32(w[3]);
    con.put(b'\n');
    hw::wdt_disable();
    let w = hw::wdt_state();
    con.puts("wdt after:  timg0 ");
    con.hex32(w[0]);
    con.puts(" timg1 ");
    con.hex32(w[1]);
    con.puts(" rwdt ");
    con.hex32(w[2]);
    con.puts(" swd ");
    con.hex32(w[3]);
    con.put(b'\n');
    con.line("step: flash size");
    con.puts("config_param rc ");
    con.hex32(hw::flash_set_size() as u32);
    con.put(b'\n');
    let mut flash = hw::RomFlash;
    boot_core::proof::run(&mut flash, &mut con, &MEMORY_MAP, &mut || {});
    Uart::drain();
    park()
}

fn park() -> ! {
    loop {
        // SAFETY: wfi has no memory effect.
        unsafe { core::arch::asm!("wfi") };
    }
}

#[no_mangle]
extern "C" fn stage_trap(mcause: u32, mepc: u32, mtval: u32) -> ! {
    let mut con = Uart;
    con.puts("\nLBOOT TRAP mcause ");
    con.hex32(mcause);
    con.puts(" mepc ");
    con.hex32(mepc);
    con.puts(" mtval ");
    con.hex32(mtval);
    con.put(b'\n');
    Uart::drain();
    park()
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    let mut con = Uart;
    con.line("\nLBOOT PANIC");
    Uart::drain();
    park()
}
