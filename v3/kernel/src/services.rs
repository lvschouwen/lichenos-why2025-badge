//! Core 0's services for core 1's processes (#619): what a forwarded syscall reaches. Called from `Link::call` while
//! core 0 waits on core 1 (a process stage), never from a trap. One service today, the console for WRITE.
//!
//! The console: a forwarded WRITE is printed from the caller's bounce buffer (`forward::bounce`), never from a U
//! address, on lines of its own prefixed `[c1 <name>] ` (`Lines`). It is printed only when the log ring keeps
//! RESERVE bytes free afterwards (`fits`); until then core 0 holds the reply, so the caller stays blocked and a flood
//! runs at the UART's rate (about 11 bytes per ms) instead of pushing the kernel's rows out of the ring. Core 0 never
//! busy-waits for that room: `poll` retries between its mailbox polls.
use crate::forward;
use crate::global::Global;
use crate::kprint;
use crate::platform;
use crate::sched::MAX_PROCS;
use crate::smp::{Link, Msg, OP_SYS, REPLY};
use lichen_abi::{err, sys, WRITE_MAX};
use lichen_kcore::forward::{fits, Lines};

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
    held: [Option<Msg>; MAX_PROCS],
    stats: Stats,
}

static SVC: Global<ConsoleSvc> = Global::new(ConsoleSvc {
    lines: Lines::new(),
    held: [None; MAX_PROCS],
    stats: Stats { served: 0, bytes: 0, held: 0, dropped: 0, refused: 0 },
});

fn svc() -> &'static mut ConsoleSvc {
    // SAFETY: core 0 only (Link is core 0's), called from Link::call outside any trap; each caller's borrow ends
    // before the next call here.
    unsafe { SVC.get() }
}

/// A request from core 1, popped by `Link::call`.
pub fn request(l: &mut Link, m: Msg) {
    let (i, nr, len) = (m.a[0] as usize, m.a[1] as usize, m.a[2] as usize);
    let bad = if m.op != OP_SYS || i >= MAX_PROCS {
        Some(err::EINVAL)
    } else if nr != sys::WRITE {
        Some(err::ENOSYS)
    } else if len > WRITE_MAX || svc().held[i].is_some() {
        // A second request from a process with one held breaks the one-in-flight rule: refuse it, keep the first.
        Some(err::EINVAL)
    } else {
        None
    };
    if let Some(e) = bad {
        svc().stats.refused += 1;
        answer(l, m, e);
        return;
    }
    if !serve(l, m) {
        let s = svc();
        s.held[i] = Some(m);
        s.stats.held += 1;
    }
}

/// Between mailbox polls: retry the held WRITEs once the UART made room.
pub fn poll(l: &mut Link) {
    platform::console_pump();
    for i in 0..MAX_PROCS {
        if let Some(m) = svc().held[i] {
            if serve(l, m) {
                svc().held[i] = None;
            }
        }
    }
}

/// The call is over: a held WRITE has nobody waiting any more (core 1's stage ended), and an open app line is ended,
/// so the next kernel row starts a line of its own. Anything printed goes out before core 0 prints rows again.
pub fn settle() {
    let s = svc();
    for h in &mut s.held {
        if h.take().is_some() {
            s.stats.dropped += 1;
        }
    }
    if s.lines.close() {
        kprint!("\n");
    }
    if s.stats.bytes != 0 {
        platform::flush();
    }
}

pub fn take_stats() -> Stats {
    core::mem::take(&mut svc().stats)
}

/// Print `m`'s bytes and answer it, if the console has room for them; false (nothing done) if not.
fn serve(l: &mut Link, m: Msg) -> bool {
    let i = m.a[0] as usize;
    // SAFETY: core 1 pushed `m` after filling process i's buffer and does not touch it until this request's reply.
    let b = unsafe { forward::bounce(i) };
    let bytes = &b.buf[..(m.a[2] as usize).min(WRITE_MAX)];
    let name = b.name;
    let s = svc();
    let cost = s.lines.cost(i, bytes, name.len() + PREFIX_EXTRA);
    if !fits(cost, platform::console_free(), RESERVE) {
        return false;
    }
    s.lines.render(i, bytes, || kprint!("[c1 {name}] "), |c| kprint!("{}", c as char));
    s.stats.served += 1;
    s.stats.bytes = s.stats.bytes.saturating_add(cost as u32);
    answer(l, m, bytes.len() as isize);
    true
}

fn answer(l: &mut Link, m: Msg, ret: isize) {
    let r = Msg { op: m.op | REPLY, seq: m.seq, a: [m.a[0], ret as i32 as u32, 0, 0, 0, 0] };
    if !l.answer(r) {
        svc().stats.refused += 1;
    }
}
