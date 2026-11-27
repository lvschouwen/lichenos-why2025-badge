//! App image loader. An image is linked for one slot (fixed addresses, see apps/app.ld.in) and starts with a header:
//! magic, ABI version, entry, heap start, image end. Everything in the header is checked against the slot before a
//! single byte is copied; a bad image is refused and nothing runs.
use crate::sched::{Kernel, Slot, State, Task};
use crate::trap::Context;
use lichen_abi::{ABI_VERSION, IMAGE_MAGIC};

/// Room kept free for the app's stack at the top of its data region; sbrk may not grow into it.
pub const STACK_RESERVE: usize = 4096;

#[derive(Debug, PartialEq, Eq)]
pub enum LoadError {
    TooSmall,
    TooBig,
    Magic,
    Abi,
    Entry,
    Heap,
    ImageEnd,
}

fn word(b: &[u8], i: usize) -> usize {
    u32::from_le_bytes([b[4 * i], b[4 * i + 1], b[4 * i + 2], b[4 * i + 3]]) as usize
}

pub fn validate(blob: &[u8], s: &Slot) -> Result<(usize, usize), LoadError> {
    if blob.len() < 20 {
        return Err(LoadError::TooSmall);
    }
    if blob.len() > s.len {
        return Err(LoadError::TooBig);
    }
    if word(blob, 0) != IMAGE_MAGIC as usize {
        return Err(LoadError::Magic);
    }
    if word(blob, 1) != ABI_VERSION as usize {
        return Err(LoadError::Abi);
    }
    let (entry, heap, end) = (word(blob, 2), word(blob, 3), word(blob, 4));
    if entry < s.text + 20 || entry >= s.text + blob.len() || entry % 2 != 0 {
        return Err(LoadError::Entry);
    }
    if heap < s.data || heap > s.data + s.len - STACK_RESERVE {
        return Err(LoadError::Heap);
    }
    if end != s.text + blob.len() {
        return Err(LoadError::ImageEnd);
    }
    Ok((entry, heap))
}

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
    crate::platform::sync_icache();
    let mut ctx = Context::ZERO;
    ctx.pc = entry;
    ctx.regs[2] = slot.data + slot.len; // sp
    ctx.regs[10..14].copy_from_slice(&args);
    k.tasks[i] = Task { state: State::Ready, ctx, name, slot, heap_start: heap, brk: heap, budget, ..Task::EMPTY };
    Ok(())
}
