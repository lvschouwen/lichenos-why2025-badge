//! HP GPIO (pins 0..31) as plain outputs, via the GPIO matrix and IO_MUX.
use crate::mmio::{modify32, read32, write32};

const GPIO: usize = 0x500E_0000; // DR_REG_GPIO_BASE
const OUT_W1TS: usize = GPIO + 0x08;
const OUT_W1TC: usize = GPIO + 0x0C;
const ENABLE_W1TS: usize = GPIO + 0x24;
const IN: usize = GPIO + 0x3C;
const FUNC_OUT_SEL_CFG: usize = GPIO + 0x558; // + 4 * pin
const IO_MUX: usize = 0x500E_1000; // DR_REG_IO_MUX_BASE; pin n at + 4 + 4 * n
const SIG_GPIO_OUT: u32 = 256; // "simple GPIO output": the pin follows GPIO_OUT
const MCU_SEL_MASK: u32 = 0x7 << 12;
const MCU_SEL_GPIO: u32 = 1 << 12;
const FUN_IE: u32 = 1 << 9;

fn check_pin(pin: u32) {
    assert!(pin < 32, "gpio: only pins 0..31 are in the first register bank");
}

/// Make `pin` a push-pull output at `level`, with the input path on so `level_in` reads the pad back.
/// The level is latched before the output is enabled, so the pad never glitches through the other level.
pub fn output(pin: u32, level: bool) {
    check_pin(pin);
    // SAFETY: GPIO matrix / IO_MUX / GPIO registers of this chip for a valid pin; this is the pin's whole setup.
    unsafe {
        write32(if level { OUT_W1TS } else { OUT_W1TC }, 1 << pin);
        write32(FUNC_OUT_SEL_CFG + 4 * pin as usize, SIG_GPIO_OUT);
        modify32(IO_MUX + 4 + 4 * pin as usize, MCU_SEL_MASK, MCU_SEL_GPIO | FUN_IE);
        write32(ENABLE_W1TS, 1 << pin);
    }
}
/// The level on the pad (needs the input path on, which `output` does).
pub fn level_in(pin: u32) -> bool {
    check_pin(pin);
    // SAFETY: GPIO_IN, a read without side effects.
    let v = unsafe { read32(IN) };
    v & (1 << pin) != 0
}
