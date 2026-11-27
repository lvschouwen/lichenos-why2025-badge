//! ESP32-P4 rev v1.0 hardware layer, hand-written from the v1.0 register headers (hw_ver1), no esp-hal. Each module
//! owns one block. Everything that touches an address goes through `mmio`, so the unsafe surface stays in one file.
//! The M1-B modules (trap, clic, clint, pmp, pma, core1, mailbox) belong to #612 (trap, clic, clint filled; the rest are
//! stubs) and nothing else
//! edits this list.
#![no_std]

pub mod c6;
pub mod cache;
pub mod clock;
pub mod gpio;
pub mod mem;
pub mod mmio;
// Owned by #620.
pub mod psram;
pub mod uart;

// Stubs for #612.
pub mod clic;
pub mod clint;
pub mod core1;
pub mod mailbox;
pub mod pma;
pub mod pmp;
pub mod trap;
