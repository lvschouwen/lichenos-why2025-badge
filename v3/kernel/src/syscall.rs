//! System call dispatch. Every pointer is checked against the caller's own regions with overflow-safe arithmetic,
//! and the bytes are copied into a kernel buffer before use. On core 1 a WRITE is forwarded to core 0 (#619): the
//! range is checked and copied here on core 1, and core 0 sees only the kernel's copy, never the U pointer.
use crate::kprint;
use crate::loader::STACK_RESERVE;
use crate::sched::{end_proc, End, Kernel, Pcb};
use lichen_abi::{err, sys, WRITE_MAX};
use lichen_kcore::forward::in_region;

fn user_range_ok(t: &Pcb, ptr: usize, len: usize) -> bool {
    in_region(ptr, len, t.slot.text, t.slot.len) || in_region(ptr, len, t.slot.data, t.slot.len)
}

/// Core 1 has no console of its own (its UART path is a raw busy-wait, about 23 ms for a full WRITE with interrupts
/// off): its WRITE goes to core 0's console service over the mailbox (#619, `forward`). Returns whether the caller
/// gives up the CPU, or None where WRITE prints locally (core 0, qemu-virt).
fn forward_write(k: &mut Kernel, cur: usize, ptr: usize, len: usize) -> Option<bool> {
    #[cfg(feature = "esp32p4")]
    if k.sup == crate::sched::Supervisor::Procs {
        return Some(crate::forward::write(k, cur, ptr, len));
    }
    let _ = (k, cur, ptr, len);
    None
}

/// Returns true when the caller gives up the CPU.
pub fn dispatch(k: &mut Kernel, cur: usize) -> bool {
    let r = k.procs[cur].ctx.regs;
    let (n, a0, a1) = (r[17], r[10], r[11]);
    let ret: isize = match n {
        sys::EXIT => {
            end_proc(k, cur, End::Exit(a0 as isize));
            return true;
        }
        sys::WRITE => {
            if a1 > WRITE_MAX {
                err::EINVAL
            } else if !user_range_ok(&k.procs[cur], a0, a1) {
                err::EFAULT
            } else if let Some(gives_up) = forward_write(k, cur, a0, a1) {
                return gives_up;
            } else {
                let mut buf = [0u8; WRITE_MAX];
                // SAFETY: [a0, a0+a1) lies inside the caller's own slot (checked above), which is mapped RAM.
                unsafe { core::ptr::copy_nonoverlapping(a0 as *const u8, buf.as_mut_ptr(), a1) };
                kprint!("[{}] ", k.procs[cur].name);
                for &b in &buf[..a1] {
                    if b == b'\n' || (0x20..0x7F).contains(&b) {
                        kprint!("{}", b as char);
                    }
                }
                a1 as isize
            }
        }
        sys::YIELD => {
            k.procs[cur].ctx.regs[10] = 0;
            return true;
        }
        sys::TICKS => k.ticks as isize,
        sys::SBRK => {
            let t = &mut k.procs[cur];
            let limit = t.slot.data + t.slot.len - STACK_RESERVE;
            let incr = a0 as isize;
            match t.brk.checked_add_signed(incr) {
                Some(nb) if nb < t.heap_start => err::EINVAL,
                Some(nb) if nb <= limit => {
                    let old = t.brk;
                    t.brk = nb;
                    old as isize
                }
                Some(_) => err::ENOMEM,
                None => err::EINVAL,
            }
        }
        sys::HANDLE_OPEN => k.handles.open(cur),
        sys::HANDLE_USE => k.handles.check(cur, a0),
        sys::HANDLE_CLOSE => k.handles.close(cur, a0),
        _ => err::ENOSYS,
    };
    k.procs[cur].ctx.regs[10] = ret as usize;
    false
}
