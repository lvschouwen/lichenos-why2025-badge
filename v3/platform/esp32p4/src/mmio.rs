//! Volatile 32-bit register access, the only place this crate dereferences a raw address.

/// Read a 32-bit register.
///
/// # Safety
/// `addr` must be a 4-byte aligned address that is valid to read in M-mode (a register of this chip or mapped RAM).
#[inline(always)]
pub unsafe fn read32(addr: usize) -> u32 {
    core::ptr::read_volatile(addr as *const u32)
}

/// Write a 32-bit register.
///
/// # Safety
/// `addr` must be a 4-byte aligned address that is valid to write in M-mode, and the write must be allowed to happen
/// at this point of the boot (it may reconfigure hardware).
#[inline(always)]
pub unsafe fn write32(addr: usize, v: u32) {
    core::ptr::write_volatile(addr as *mut u32, v)
}

/// Read-modify-write: clear `clear`, then set `set`.
///
/// # Safety
/// As `read32` and `write32` for the same `addr`.
pub unsafe fn modify32(addr: usize, clear: u32, set: u32) {
    write32(addr, (read32(addr) & !clear) | set)
}
