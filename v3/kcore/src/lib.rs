//! Lichen v3 kernel pure logic. Everything here is hardware-free (no MMIO, no asm), so it runs under `cargo test` on
//! the host with proptest. The kernel and the platform crates only glue it to the machine.
#![no_std]

pub mod check;
pub mod handles;
pub mod image;
pub mod l2;
pub mod mailbox;
pub mod perf;
pub mod pma;
pub mod pmp;
pub mod ring;
pub mod slots;
pub mod timebase;
pub mod trap;
