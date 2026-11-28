//! Core 0's services for core 1's processes (#619): what a forwarded syscall reaches. Called from `Link::call` while
//! core 0 waits on core 1 (a process stage), never from a trap. One service today, the console for WRITE.
//!
//! The console: a forwarded WRITE is printed from the caller's bounce buffer (`forward::bounce`), never from a U
//! address, on lines of its own prefixed `[c1 <name>] ` (`Lines`). It is printed only when the log ring keeps RESERVE
//! bytes free afterwards (`fits`); until then core 0 holds the reply, so the caller stays blocked and a flood runs at
//! the UART's rate (the #619 bench measured about 66 bytes per ms) instead of pushing the kernel's rows out of the
//! ring. Held WRITEs are served in arrival order and a new one never overtakes them (`Fifo`), so a quiet process waits
//! behind at most one WRITE of each other process (one call in flight each). A WRITE larger than the ring could ever
//! take is cut to what fits and reports that shorter length, like a partial write. Core 0 never busy-waits for room:
//! `poll` retries between its mailbox polls.
use crate::forward;
use crate::global::Global;
use crate::kprint;
use crate::platform;
use crate::sched::MAX_PROCS;
use crate::smp::{Link, Msg, OP_SYS, REPLY};
use lichen_abi::{err, sys, WRITE_MAX};
use lichen_kcore::forward::{fits, Fifo, Lines};

/// Log ring bytes a forwarded WRITE must leave free: the rows core 0 prints after a stage fit in it.
pub const RESERVE: usize = 1024;
/// `[c1 ` and `] ` around the caller's name.
const PREFIX_EXTRA: usize = 6;

/// What the console service did, since the last `take_stats`.
#[derive(Clone, Copy, Default, Debug)]
pub struct Stats {
    /// WRITEs printed and answered.
    pub served: u32,
    /// Bytes they put on the console (prefixes and line ends included).
    pub bytes: u32,
    /// WRITEs that had to wait for room at least once.
    pub held: u32,
    /// WRITEs still held when the call ended (the stage was over): dropped, their caller is gone.
    pub dropped: u32,
    /// Requests refused (bad process, length or syscall number) or replies core 1 never took.
    pub refused: u32,
}

struct ConsoleSvc {
    lines: Lines<MAX_PROCS>,
    held: Fifo<Msg, MAX_PROCS>,
    stats: Stats,
}

static SVC: Global<ConsoleSvc> = Global::new(ConsoleSvc {
    lines: Lines::new(),
    held: Fifo::new(),
    stats: Stats { served: 0, bytes: 0, held: 0, dropped: 0, refused: 0 },
});

/// The console service's state.
///
/// # Safety
/// Core 0 only, outside any trap (from `Link::call`, or between stages for `take_stats`), and no other reference from
/// `svc` live: each public entry takes it once and passes it down.
unsafe fn svc() -> &'static mut ConsoleSvc {
    SVC.get()
}

/// A request from core 1, popped by `Link::call`.
pub fn request(l: &mut Link, m: Msg) {
    // SAFETY: core 0 inside Link::call; the only reference for this call.
    let s = unsafe { svc() };
    let (i, nr, len) = (m.a[0] as usize, m.a[1] as usize, m.a[2] as usize);
    let bad = if m.op != OP_SYS || i >= MAX_PROCS {
        Some(err::EINVAL)
    } else if nr != sys::WRITE {
        Some(err::ENOSYS)
    } else if len > WRITE_MAX || s.held.any(|h| h.a[0] as usize == i) {
        // A second request from a process with one held breaks the one-in-flight rule: refuse it, keep the first.
        Some(err::EINVAL)
    } else {
        None
    };
    if let Some(e) = bad {
        s.stats.refused += 1;
        answer(l, s, m, e);
        return;
    }
    let m = cut_to_fit(s, m);
    // Arrival order: a new WRITE waits behind every held one, even if it would fit now.
    if s.held.is_empty() && serve(l, s, m) {
        return;
    }
    match s.held.push(m) {
        Ok(()) => s.stats.held += 1,
        Err(m) => {
            // Cannot happen (one held per process at most): refuse rather than lose it silently.
            s.stats.refused += 1;
            answer(l, s, m, err::EAGAIN);
        }
    }
}

/// Between mailbox polls: serve the held WRITEs, oldest first, as far as the UART made room. With nothing held it
/// touches no hardware, so core 0's wait loop stays what the timing rows measured.
pub fn poll(l: &mut Link) {
    // SAFETY: core 0 inside Link::call; the only reference for this call.
    let s = unsafe { svc() };
    if s.held.is_empty() {
        return;
    }
    platform::console_pump();
    while let Some(m) = s.held.front() {
        if !serve(l, s, m) {
            return;
        }
        s.held.pop();
    }
}

/// The call is over: a held WRITE has nobody waiting any more (core 1's stage ended), and an open app line is ended,
/// so the next kernel row starts a line of its own. Anything printed goes out before core 0 prints rows again.
pub fn settle() {
    // SAFETY: core 0 at the end of Link::call; the only reference for this call.
    let s = unsafe { svc() };
    while s.held.pop().is_some() {
        s.stats.dropped += 1;
    }
    if s.lines.close() {
        kprint!("\n");
    }
    if s.stats.bytes != 0 {
        platform::flush();
    }
}

pub fn take_stats() -> Stats {
    // SAFETY: core 0 between stages; the only reference for this call.
    core::mem::take(&mut unsafe { svc() }.stats)
}

/// A WRITE that even an empty ring could not take with the reserve kept (a full WRITE of newlines, each with its
/// prefix) would be held for good: cut it to what fits; the caller gets the shorter length back.
fn cut_to_fit(s: &ConsoleSvc, mut m: Msg) -> Msg {
    let i = m.a[0] as usize;
    // SAFETY: core 1 pushed `m` after filling process i's buffer and does not touch it until this request's reply.
    let b = unsafe { forward::bounce(i) };
    let prefix = b.name.unwrap_or("?").len() + PREFIX_EXTRA;
    // The line state may change before it is served: allow for its own prefix and another process's line end.
    let budget = platform::CONSOLE_RING.saturating_sub(RESERVE + prefix + 2);
    let len = (m.a[2] as usize).min(WRITE_MAX);
    m.a[2] = s.lines.fit_len(i, &b.buf[..len], prefix, budget) as u32;
    m
}

/// Print `m`'s bytes and answer it, if the console has room for them; false (nothing done) if not.
fn serve(l: &mut Link, s: &mut ConsoleSvc, m: Msg) -> bool {
    let i = m.a[0] as usize;
    // SAFETY: as in `cut_to_fit`.
    let b = unsafe { forward::bounce(i) };
    let bytes = &b.buf[..(m.a[2] as usize).min(WRITE_MAX)];
    let name = b.name.unwrap_or("?");
    let cost = s.lines.cost(i, bytes, name.len() + PREFIX_EXTRA);
    if !fits(cost, platform::console_free(), RESERVE) {
        return false;
    }
    s.lines.render(i, bytes, || kprint!("[c1 {name}] "), |c| kprint!("{}", c as char));
    s.stats.served += 1;
    s.stats.bytes = s.stats.bytes.saturating_add(cost as u32);
    answer(l, s, m, bytes.len() as isize);
    true
}

fn answer(l: &mut Link, s: &mut ConsoleSvc, m: Msg, ret: isize) {
    let r = Msg { op: m.op | REPLY, seq: m.seq, a: [m.a[0], ret as i32 as u32, 0, 0, 0, 0] };
    if !l.answer(r) {
        // Lost: the caller stays blocked until its stage ends (see `Link::answer`).
        s.stats.refused += 1;
    }
}
