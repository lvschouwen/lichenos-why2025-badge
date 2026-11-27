//! The RAM-only memory map of the v3 bring-up image (#611), as used by `kernel/link-esp32p4.ld` and checked again on
//! the finished image by `v3/tools/check_image.py`. Keep the three files in step.
//!
//! The ROM download mode (esptool load-ram) loads segments into L2MEM and jumps to the entry. The loaded image must
//! end below `LOAD_LIMIT`; NOLOAD memory (bss, stacks, slots) starts at `NOLOAD_BASE` and
//! stops at the L2 cache carve.

/// Start of L2MEM.
pub const L2MEM_BASE: usize = 0x4FF0_0000;
/// Where esptool's rev1 stub keeps its text; load-ram is known to leave it alone. The image loads from here.
pub const LOAD_BASE: usize = 0x4FF1_0000;
/// The loaded image must end below this: the v3 boot stage (#615) is loaded by the ROM at 0x4FF28ED0..0x4FF3ABD0, so one
/// limit serves both the stage-booted and the load-ram route (the ROM stack at 0x4FF3AFC0 is above it).
pub const LOAD_LIMIT: usize = 0x4FF2_8ED0;
/// NOLOAD memory starts here: nothing the ROM writes while loading reaches it.
pub const NOLOAD_BASE: usize = 0x4FF4_0000;
/// NOLOAD memory ends here: the carve of the 256 KB cache that the P4 reset value implies (0x4FFC0000 - 256 KB).
/// The kernel reads the real size at boot and CHECKs it.
pub const NOLOAD_LIMIT: usize = 0x4FF8_0000;
