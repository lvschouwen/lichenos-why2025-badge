//! System call dispatch. Every pointer is checked against the caller's own regions with overflow-safe arithmetic,
//! and the bytes are copied into a kernel buffer before use. (Single hart and the app is stopped during the call,
//! so there is no TOCTOU window today; with a second core the copy-first rule is what keeps it safe.)
use crate::kprint;
use crate::loader::STACK_RESERVE;
use crate::sched::{end_proc, End, Kernel, Pcb};
use lichen_abi::{err, sys, WRITE_MAX};

fn in_region(ptr: usize, len: usize, base: usize, size: usize) -> bool {
    match ptr.checked_add(len) {
        Some(end) => ptr >= base && end <= base + size,
        None => false,
    }
}
fn user_range_ok(t: &Pcb, ptr: usize, len: usize) -> bool {
    in_region(ptr, len, t.slot.text, t.slot.len) || in_region(ptr, len, t.slot.data, t.slot.len)
}

/// Core 1's console is the raw busy-wait UART (about 23 ms for a full WRITE with interrupts off): a process there could
/// stall the hart and dodge its budget with it, so WRITE is denied until core 0 offers a console service (#618).
fn denied_write(k: &Kernel) -> bool {
    #[cfg(feature = "esp32p4")]
    return k.sup == crate::sched::Supervisor::Procs;
    #[cfg(not(feature = "esp32p4"))]
    {
        let _ = k;
        false
    }
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
            if denied_write(k) {
                err::EPERM
            } else if a1 > WRITE_MAX {
                err::EINVAL
            } else if !user_range_ok(&k.procs[cur], a0, a1) {
                err::EFAULT
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
