//! Chip-independent core of the Lichen v3 boot stage (#615). `no_std`, no heap, no `core::fmt` (the stage has to fit
//! the 24 KB the ROM reads from flash), host-tested with proptest. The chip crate supplies the three things that touch
//! hardware: a `FlashRead`, a `Console` and a watchdog tick.
//!
//! Invariants: nothing in here writes flash or touches the eFuse controller; a flash read is the only I/O.
#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]

pub mod bootinfo;
pub mod console;
pub mod crc;
pub mod flash;
pub mod image;
pub mod mem;
pub mod otadata;
pub mod partition;
pub mod proof;

#[cfg(test)]
extern crate std;
#[cfg(test)]
mod testutil;
