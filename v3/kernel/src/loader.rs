//! App image loader. An image is linked for one slot (fixed addresses, see apps/app.ld.in) and starts with a header:
//! magic, ABI version, entry, heap start, image end. Everything in the header is checked against the slot before a
//! single byte is copied; a bad image is refused and nothing runs. The process's PMP view is planned here too (#617).
use crate::sched::{Kernel, Pcb, State};
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
    if k.calls.busy(i) {
        return Err(LoadError::Busy);
    }
    let (entry, heap) = validate(blob, &slot)?;
    // validate refuses an image longer than its slot; checked again here, so a regression there is a refused image
    // and a FAIL row, never a wrapping length in the write_bytes below.
    let tail = slot.len.checked_sub(blob.len()).ok_or(LoadError::TooBig)?;
    // SAFETY: the slot regions are reserved for apps by the platform memory map and lie outside the kernel image;
    // task i is not running (the kernel runs), so nothing else accesses them now. Zeroing the whole data region
    // keeps a previous app's data from leaking to the next one.
    unsafe {
        core::ptr::copy_nonoverlapping(blob.as_ptr(), slot.text as *mut u8, blob.len());
        core::ptr::write_bytes((slot.text + blob.len()) as *mut u8, 0, tail);
        core::ptr::write_bytes(slot.data as *mut u8, 0, slot.len);
    }
    crate::platform::sync_icache(slot.text, slot.len);
    let mut ctx = Context::ZERO;
    ctx.pc = entry;
    ctx.regs[2] = slot.data + slot.len; // sp
    ctx.regs[10..14].copy_from_slice(&args);
    // The view is planned and checked here, once: a switch to this process only loads it (#617).
    let view = Some(crate::pmp::app_view(slot.text, slot.data, slot.len));
    k.procs[i] = Pcb {
        state: State::Ready,
        ctx,
        name,
        slot,
        view,
        heap_start: heap,
        brk: heap,
        budget,
        image_len: blob.len(),
        ..Pcb::EMPTY
    };
    Ok(())
}
