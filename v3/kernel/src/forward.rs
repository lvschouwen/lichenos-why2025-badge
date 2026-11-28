//! Syscall forwarding, core 1's side (#619). A U process on core 1 makes a service syscall (WRITE today); core 1's
//! kernel checks the range (syscall.rs), copies the bytes into the process's bounce buffer here, sends OP_SYS to core
//! 0 and blocks the caller. Core 0's `services` answers on TO_C1; `drain` (after every trap on core 1, and in the
//! short wait below) makes the caller Ready with the result in a0. Core 1 touches no hardware for this: the queues and
//! the bounce buffers are L2MEM it already uses (it stays on the ROM PMA, see `smp`).
//!
//! Fairness, the design:
//! - One call in flight per process (`Calls`): a blocked caller cannot trap again, so a flood is one request at a
//!   time, and TO_C0 never holds more than MAX_PROCS of them.
//! - A blocked caller does not run; the others do. A reply only makes the caller Ready, it does not preempt: the
//!   caller runs again at its next turn in the round robin. So a process that floods gets one call per turn at most,
//!   and gives up the rest of its turn with each call.
//! - The kernel's work for a call (trap, check, copy, push) is billed to the caller (`bill_service`), so a flood is
//!   metered and its cycle budget still ends it.
//! - Core 0 holds a WRITE while its console is short of room (`services`): the caller stays blocked, so the flood
//!   goes at the UART's rate (about 66 bytes per ms on the #619 bench) and never pushes the kernel's rows out of the log ring.
//! - Latency: when no other process can run, core 1 waits for the reply inside the trap for up to FAST_WAIT_US
//!   (`fast` in the counts) instead of switching to idle, which would leave the reply until the next tick. The wait
//!   is billed to the caller whether the reply came or not.
//! - A process that ends with a call in flight leaves its slot lent (`Calls::cancel`): core 0 may still read its
//!   bounce buffer, so no call begins there and nothing loads there until that call's reply or the stage end.
use crate::global::Global;
use crate::platform::hw;
use crate::sched::{Kernel, Samples, State, MAX_PROCS};
use crate::smp::{self, Msg, OP_SYS, REPLY};
use crate::{csr, timebase};
use lichen_abi::{err, sys, WRITE_MAX};
use lichen_kcore::account::bill_service;

/// How long core 1 waits in the trap for a reply when nothing else can run: a few round trips (the #619 bench measured
/// a forwarded WRITE at 30.7 us median, 32.7 us p95 at 40 MHz), so a reply core 0 holds back does not keep the trap
/// (and core 1's tick) waiting; that call is answered at a later trap instead. The spin is billed to the caller.
const FAST_WAIT_US: u64 = 50;

/// One process's bounce buffer: core 1 fills it before the OP_SYS push (Release), core 0 reads it after its pop
/// (Acquire) and is done with it before it pushes the reply; core 1 touches it again only after that reply (the caller
/// is blocked until then, one call in flight per process).
pub struct Bounce {
    /// The caller's name for core 0's line prefix. An Option so the table is all zero bytes and stays in .bss (an
    /// empty `&str` is a non-null pointer and would put 804 B into the loaded image's .data).
    pub name: Option<&'static str>,
    pub len: usize,
    pub buf: [u8; WRITE_MAX],
}

/// The bounce buffers, by core 1 process index. Ownership moves with the mailbox messages (see `Bounce`), which is the
/// only reason a `Global` shared by two harts is sound here.
static BOUNCE: [Global<Bounce>; MAX_PROCS] =
    [const { Global::new(Bounce { name: None, len: 0, buf: [0; WRITE_MAX] }) }; MAX_PROCS];

/// Process `i`'s bounce buffer.
///
/// # Safety
/// The caller owns it now: core 1 while process `i` has no call in flight, core 0 between the OP_SYS pop and its reply.
pub unsafe fn bounce(i: usize) -> &'static mut Bounce {
    BOUNCE[i].get()
}

/// Round-trip latency samples on core 1: trap entry stamp of the call to the caller's resume with its result.
pub struct FwdPerf {
    /// Answered while the caller waited in the trap (nothing else could run).
    pub fast: Samples,
    /// Answered while other processes ran: the caller resumed at its next turn.
    pub slow: Samples,
}
static PERF: Global<FwdPerf> = Global::new(FwdPerf { fast: Samples::new(), slow: Samples::new() });

/// Core 1's samples.
///
/// # Safety
/// Core 1's trap path during a stage, or core 0 while core 1 waits in its mailbox loop (before OP_PROCS, after its
/// reply); no other live reference.
pub unsafe fn perf() -> &'static mut FwdPerf {
    PERF.get()
}

/// WRITE from process `cur` on core 1, its range already checked. Returns whether the caller gives up the CPU.
pub fn write(k: &mut Kernel, cur: usize, ptr: usize, len: usize) -> bool {
    let Some(seq) = k.calls.begin(cur) else {
        // Cannot happen (a blocked caller does not trap): refuse rather than overwrite a buffer core 0 may be reading.
        k.procs[cur].ctx.regs[10] = err::EAGAIN as usize;
        return false;
    };
    // SAFETY: `cur` has no call in flight (begin above), so core 1 owns its bounce buffer.
    let b = unsafe { bounce(cur) };
    // SAFETY: [ptr, ptr + len) lies inside the caller's own slot (checked by dispatch), mapped L2MEM, len <= WRITE_MAX.
    unsafe { core::ptr::copy_nonoverlapping(ptr as *const u8, b.buf.as_mut_ptr(), len) };
    b.name = Some(k.procs[cur].name);
    b.len = len;
    let m = Msg { op: OP_SYS, seq, a: [cur as u32, sys::WRITE as u32, len as u32, 0, 0, 0] };
    // SAFETY: core 1's trap path during a stage; the mailbox loop holds no borrow meanwhile.
    if unsafe { smp::c1() }.tx.push(m).is_err() {
        k.calls.withdraw(cur);
        k.fwd.refused += 1;
        k.procs[cur].ctx.regs[10] = err::EAGAIN as usize;
        return false;
    }
    k.fwd.sent += 1;
    let p = &mut k.procs[cur];
    p.state = State::Blocked;
    p.call_entry = p.ctx.entry_cycle as u32;
    p.call_woken = false;
    bill_service(&mut p.meter, &mut k.kernel_time, csr::mcycle() as u32);
    if others_ready(k, cur) {
        return true;
    }
    let deadline = hw::clint::mtime() + timebase::us_to_ticks(FAST_WAIT_US);
    while hw::clint::mtime() < deadline {
        drain(k);
        if k.procs[cur].state == State::Ready {
            k.procs[cur].call_woken = false;
            k.fwd.fast += 1;
            let c = (csr::mcycle() as u32).wrapping_sub(k.procs[cur].call_entry);
            note(&mut k.procs[cur], c);
            // SAFETY: core 1's trap path, the only reference.
            unsafe { perf() }.fast.push(c);
            return false;
        }
        if others_ready(k, cur) {
            break;
        }
    }
    // No reply in time: the spin was for this caller, so it is the caller's (review M1), not the kernel's.
    let p = &mut k.procs[cur];
    bill_service(&mut p.meter, &mut k.kernel_time, csr::mcycle() as u32);
    true
}

/// One finished call of `p`: its round trip in cycles.
fn note(p: &mut crate::sched::Pcb, cycles: u32) {
    p.call_n += 1;
    p.call_max = p.call_max.max(cycles);
}

/// Another process (not idle) could run now.
fn others_ready(k: &Kernel, cur: usize) -> bool {
    (0..MAX_PROCS).any(|i| i != cur && Some(i) != k.idle && k.procs[i].state == State::Ready)
}

/// Take every reply core 0 sent: each wakes its caller with the result in a0. A reply for no call in flight (its
/// process ended, or a stale seq) is counted and dropped. Core 1's trap path only.
pub fn drain(k: &mut Kernel) {
    loop {
        // SAFETY: core 1's trap path during a stage; the borrow ends with this statement.
        let Some(r) = (unsafe { smp::c1() }).rx.pop() else { return };
        let i = r.a[0] as usize;
        if r.op != OP_SYS | REPLY || !k.calls.complete(i, r.seq) {
            if r.op != OP_SYS | REPLY {
                k.calls.stale = k.calls.stale.saturating_add(1);
            }
            continue;
        }
        let p = &mut k.procs[i];
        if p.state == State::Blocked {
            p.ctx.regs[10] = r.a[1] as i32 as isize as usize;
            p.state = State::Ready;
            p.call_woken = true;
        }
    }
}

/// `sched::resume` hands the CPU to process `i` at `now`: if a reply woke it while others ran, that call's round trip
/// ends here.
pub fn resumed(k: &mut Kernel, i: usize, now: u32) {
    let p = &mut k.procs[i];
    if p.call_woken {
        p.call_woken = false;
        k.fwd.slow += 1;
        let c = now.wrapping_sub(p.call_entry);
        note(p, c);
        // SAFETY: core 1's trap path (resume runs inside on_trap), the only reference.
        unsafe { perf() }.slow.push(c);
    }
}
