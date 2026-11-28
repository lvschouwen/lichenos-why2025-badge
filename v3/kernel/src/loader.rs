//! App image loader. An image is linked for one slot (fixed addresses, see apps/app.ld.in) and starts with a header:
//! magic, ABI version, entry, heap start, image end. Everything in the header is checked against the slot before a
//! single byte is copied; a bad image is refused and nothing runs.
use crate::sched::{Kernel, State, Task};
use crate::trap::Context;

pub use lichen_kcore::image::{validate, LoadError, Slot, STACK_RESERVE};

pub fn load(
    k: &mut Kernel,
    i: usize,
    name: &'static str,
    blob: &[u8],
    slot: Slot,
    args: [usize; 4],
    budget: Option<u32>,
) -> Result<(), LoadError> {
    let (entry, heap) = validate(blob, &slot)?;
    // SAFETY: the slot regions are reserved for apps by the platform memory map and lie outside the kernel image;
    // task i is not running (the kernel runs), so nothing else accesses them now. Zeroing the whole data region
    // keeps a previous app's data from leaking to the next one.
    unsafe {
        core::ptr::copy_nonoverlapping(blob.as_ptr(), slot.text as *mut u8, blob.len());
        core::ptr::write_bytes((slot.text + blob.len()) as *mut u8, 0, slot.len - blob.len());
        core::ptr::write_bytes(slot.data as *mut u8, 0, slot.len);
    }
    crate::platform::sync_icache(slot.text, slot.len);
    let mut ctx = Context::ZERO;
    ctx.pc = entry;
    ctx.regs[2] = slot.data + slot.len; // sp
    ctx.regs[10..14].copy_from_slice(&args);
    k.tasks[i] = Task { state: State::Ready, ctx, name, slot, heap_start: heap, brk: heap, budget, ..Task::EMPTY };
    Ok(())
}
