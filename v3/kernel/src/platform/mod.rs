#[cfg(feature = "esp32p4")]
mod esp32p4;
#[cfg(feature = "esp32p4")]
pub use esp32p4::*;

#[cfg(not(feature = "esp32p4"))]
mod qemu_virt;
#[cfg(not(feature = "esp32p4"))]
pub use qemu_virt::*;
