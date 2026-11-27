use core::cell::UnsafeCell;

/// Kernel-global state without locks.
///
/// Sound only because the PoC runs on one hart and the kernel is not re-entrant: kernel code runs at boot or inside
/// trap_handler with interrupts off (M-mode traps go to kernel_fault and halt). So at most one `&mut T` is live.
/// A second core or nested traps would break this and need a real lock.
pub struct Global<T>(UnsafeCell<T>);
// SAFETY: see the type's invariant above.
unsafe impl<T> Sync for Global<T> {}
impl<T> Global<T> {
    pub const fn new(v: T) -> Self {
        Self(UnsafeCell::new(v))
    }
    /// # Safety
    /// The caller must not hold another reference obtained from this Global (see the type invariant).
    #[allow(clippy::mut_from_ref)]
    pub unsafe fn get(&self) -> &mut T {
        &mut *self.0.get()
    }
}
