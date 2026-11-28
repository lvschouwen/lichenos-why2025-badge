//! The second HP core (#612 checkpoint 3): its start, the two mailbox queues, and the loop core 1 runs.
//!
//! Core 1 runs the same trap vector as core 0 with per-hart state (boot_esp32p4.S indexes k_ctx and the fault stack
//! by mhartid, `_start1` gives it its own kernel and trap stacks, mtrap keeps one State per hart). Its tick is its own
//! CLINT compare and CLIC line 7, both programmed through core 1's own view of those blocks.
//!
//! Console rule: the console ring is a single-context `Global` that only core 0 uses. Core 1 always writes straight
//! to the UART (platform `raw`), so its banner before UP and its failure paths at any time never touch the ring
//! (review M5). After UP core 1 only answers messages, and core 0 prints the results. `exit` on hart 1 posts its
//! code (`post_exit`) and parks; core 0's next mailbox wait sees it and ends the run with END fatal.
//!
//! The message set is the bench's, plus one request the other way (#619): OP_SYS, a syscall a U process on core 1
//! made, forwarded by core 1's kernel (`forward`). Core 0 answers it from inside `Link::call` (`services`), so a stage
//! that core 0 waits on can use core 0's services; the reply goes back on TO_C1, where core 1's trap path (not its
//! mailbox loop) takes it. `Link::call` matches its own replies by sequence number, dropping stale ones after a
//! timeout (review L1); core 1's loop drops any reply it finds (an answer to a call whose stage already ended).
//!
//! For #619 (review L6): the PMA is programmed on core 0 only (bringup_pmp). Core 1 still runs on the ROM PMA, which
//! is enough for L2MEM, and forwarding keeps core 1 there (the bounce buffers are kernel bss); before core 1 touches
//! flash or PSRAM, or runs U code that does, it must program its own PMA (the PMA CSRs are per hart, like the PMP).
use crate::kprintln;
use crate::mtrap;
use crate::platform::{self, hw};
use lichen_kcore::mailbox::{Consumer, Producer, Spsc};

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Msg {
    pub op: u32,
    pub seq: u32,
    pub a: [u32; 6],
}

/// Core 1 -> core 0, once: a = [mhartid, mtvec, pmpcfg0, pmpcfg1, pmpcfg2, pmpcfg3].
pub const OP_UP: u32 = 1;
/// Echo: the reply carries `!a[i]`, so it proves core 1 read the request.
pub const OP_PING: u32 = 2;
/// Store a[0] to `COHERE_WORD` through the cached alias, then reply.
pub const OP_COHERE: u32 = 3;
/// Run a[1] ticks of a[0] us on core 1's own timer. Reply a = [count, gap_min, gap_max, lat_max, missed, stray].
pub const OP_TICK: u32 = 4;
/// Spin a[0] iterations in U under core 1's PMP with a 100 us tick, then the illegal word.
/// Reply a = [preempts, fault code (0xFFF none), fault epc, ticks, exit a0 or 0, 1 if it exited].
pub const OP_USPIN: u32 = 5;
/// Reply, then park in wfi for good.
pub const OP_PARK: u32 = 6;
/// Run the U stub at a[0] (it must lie on the U stub page) with a0 = a[1] on core 1 under its U view, run limit a[2]
/// ms (0: run_u's own). Reply a = [fault code (0xFFF none), mtval, epc, exit a0, 1 if it exited, bit 0 timed out |
/// mintstatus.mil after the run << 8]; all u32::MAX for a bad entry.
pub const OP_URUN: u32 = 7;
/// Core 1's U-visible boot state. Reply a = [HWLP state 0x7f1, PIE state 0x7f2, mcounteren, mstatus, 0, 0].
pub const OP_BOOT: u32 = 8;
/// Run the process stage core 0 prepared in core 1's table (#617, `procs::stage`): a[0] deadline ms, a[1] idle
/// ticks to wait for (0: the defaults). Reply: see `procs::stage`.
pub const OP_PROCS: u32 = 9;
/// Core 1 -> core 0 (#619): process a[0] on core 1 made syscall a[1]; for WRITE a[2] is the length of the bytes in
/// that process's bounce buffer (`forward`). Reply (on TO_C1) a = [process, result as isize, 0, 0, 0, 0].
pub const OP_SYS: u32 = 10;
/// Set on every reply.
pub const REPLY: u32 = 0x8000_0000;

const QLEN: usize = 16;
/// Core 0 produces, core 1 consumes.
static TO_C1: Spsc<Msg, QLEN> = Spsc::new();
/// Core 1 produces, core 0 consumes.
static TO_C0: Spsc<Msg, QLEN> = Spsc::new();
/// The word OP_COHERE stores to (bench evidence for the shared L1 DCache).
pub static COHERE_WORD: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

extern "C" {
    fn _start1();
    fn u_spin_illegal();
    static _utext: u8;
}

const UPAGE: usize = 4096;
/// How long core 0 waits for room in TO_C1 to answer a core 1 request: core 1 drains it at every trap.
const ANSWER_US: u64 = 10_000;
const U_TICK_US: u64 = 100;

pub fn hartid() -> usize {
    let v: usize;
    // SAFETY: mhartid is a read-only CSR with no side effects.
    unsafe { core::arch::asm!("csrr {0}, mhartid", out(reg) v, options(nomem, nostack)) };
    v
}

/// mcycle, low word (wraps every ~10 s at 400 MHz; callers time far shorter spans with wrapping_sub). Not `nomem`:
/// a timing read must stay where it is relative to the mailbox accesses it brackets.
pub fn cycles() -> u32 {
    let v: u32;
    // SAFETY: reading mcycle has no side effects.
    unsafe { core::arch::asm!("csrr {0}, mcycle", out(reg) v, options(nostack)) };
    v
}

/// mcountinhibit (CSR 0x320); bit 0 set means mcycle is stopped.
pub fn mcountinhibit() -> u32 {
    let v: u32;
    // SAFETY: reading mcountinhibit has no side effects.
    unsafe { core::arch::asm!("csrr {0}, 0x320", out(reg) v, options(nomem, nostack)) };
    v
}

/// Let mcycle count on this hart.
pub fn uninhibit_mcycle() {
    // SAFETY: clearing mcountinhibit.CY only starts this hart's cycle counter.
    unsafe { core::arch::asm!("csrci 0x320, 1", options(nomem, nostack)) };
}

/// Set by `post_exit` (hart 1's `exit`): the exit code + 1, 0 while core 1 runs.
static C1_EXIT: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

/// Core 1's ends of the two queues: written once by `kmain1`, then used by its mailbox loop and, while a process
/// stage runs (the loop waits inside `procs::stage` then), by its trap path (`forward`). Core 1 only.
pub struct C1Ends {
    pub rx: Consumer<'static, Msg, QLEN>,
    pub tx: Producer<'static, Msg, QLEN>,
}
static C1: crate::global::Global<core::mem::MaybeUninit<C1Ends>> =
    crate::global::Global::new(core::mem::MaybeUninit::uninit());

/// Core 1's queue ends.
///
/// # Safety
/// Core 1 only, after `kmain1` wrote them; no other reference from `c1` is live (the mailbox loop takes its borrow per
/// message and holds none across `procs::stage`, whose traps take their own).
pub unsafe fn c1() -> &'static mut C1Ends {
    C1.get().assume_init_mut()
}

/// Hart 1 is done for good with `code`; core 0 ends the run (see the console rule).
pub fn post_exit(code: u32) {
    C1_EXIT.store(code.saturating_add(1), core::sync::atomic::Ordering::Release);
}

/// Core 0's ends of the two queues.
pub struct Link {
    tx: Producer<'static, Msg, QLEN>,
    rx: Consumer<'static, Msg, QLEN>,
    seq: u32,
    stale: u32,
}

/// Start core 1 at `_start1`. Core 0 must print nothing until `Link::recv` returns UP (see the console rule).
/// Returns the link and the stall polls, or None if core 1 stayed stalled.
pub fn start() -> Option<(Link, u32)> {
    // Everything core 0 wrote so far (bss, the console ring) is ordered before core 1 can run.
    core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
    // SAFETY: _start1 sets up its own stacks and mtvec (boot_esp32p4.S); this is the only call (bringup runs once).
    let polls = unsafe { hw::core1::start(_start1 as *const () as usize, 1_000_000) }?;
    // SAFETY: core 0 is the only producer of TO_C1 and the only consumer of TO_C0; Link is created once.
    Some((Link { tx: unsafe { TO_C1.producer() }, rx: unsafe { TO_C0.consumer() }, seq: 0, stale: 0 }, polls))
}

impl Link {
    /// Queue a request; waits (bounded by `deadline`, an mtime value) while the queue is full.
    pub fn send(&mut self, m: Msg, deadline: u64) -> bool {
        let mut m = m;
        loop {
            match self.tx.push(m) {
                Ok(()) => return true,
                Err(back) if hw::clint::mtime() < deadline => m = back,
                Err(_) => return false,
            }
        }
    }
    /// The next message from core 1, or None at `deadline`. Ends the run if core 1 has exited.
    pub fn recv(&mut self, deadline: u64) -> Option<Msg> {
        loop {
            if let Some(m) = self.rx.pop() {
                return Some(m);
            }
            self.check_alive();
            if hw::clint::mtime() >= deadline {
                return None;
            }
        }
    }
    /// Non-blocking. Ends the run if core 1 has exited.
    pub fn try_recv(&mut self) -> Option<Msg> {
        let m = self.rx.pop();
        if m.is_none() {
            self.check_alive();
        }
        m
    }
    /// A fresh sequence number, never reused within a run.
    pub fn next_seq(&mut self) -> u32 {
        self.reserve(1)
    }
    /// `n` consecutive fresh sequence numbers; returns the first.
    pub fn reserve(&mut self, n: u32) -> u32 {
        let first = self.seq.wrapping_add(1);
        self.seq = self.seq.wrapping_add(n);
        first
    }
    /// Count a reply that answered nobody (see `call`).
    pub fn drop_stale(&mut self) {
        self.stale += 1;
    }
    /// Replies dropped because their seq did not match the call waiting (late answers to timed-out calls).
    pub fn stale(&self) -> u32 {
        self.stale
    }
    /// Send `op` and wait up to `ms` for its own reply: a reply to an earlier, timed-out call is dropped and counted,
    /// so one timeout does not shift every later answer by one (review L1). Requests from core 1 (OP_SYS, #619) that
    /// arrive meanwhile are served (`services`), and whatever the services still hold is settled before it returns.
    pub fn call(&mut self, op: u32, a: [u32; 6], ms: u64) -> Option<Msg> {
        let deadline = hw::clint::mtime() + crate::timebase::us_to_ticks(ms * 1000);
        let seq = self.next_seq();
        if !self.send(Msg { op, seq, a }, deadline) {
            return None;
        }
        let r = loop {
            if let Some(r) = self.try_recv() {
                if r.op & REPLY == 0 {
                    crate::services::request(self, r);
                } else if r.op == op | REPLY && r.seq == seq {
                    break Some(r);
                } else {
                    self.stale += 1;
                }
                continue;
            }
            crate::services::poll(self);
            if hw::clint::mtime() >= deadline {
                break None;
            }
        };
        crate::services::settle();
        r
    }
    /// A reply to a core 1 request (`services`), bounded like `send`. False if core 1 never made room.
    pub fn answer(&mut self, m: Msg) -> bool {
        let deadline = hw::clint::mtime() + crate::timebase::us_to_ticks(ANSWER_US);
        self.send(m, deadline)
    }
    /// Core 1 posted an exit: report it and end the run here, so END is the last line.
    ///
    /// Limits (review L-g): core 0 notices only inside a mailbox wait (`recv`, `try_recv`), so core 1's exit while
    /// core 0 runs other rows shows up at core 0's next wait, or not at all after `core1.park` (core 1 is parked
    /// then and posts nothing). And a fatal path on either hart (`enter_fatal`) switches the whole console to raw
    /// output: bytes still queued in core 0's ring at that moment are not sent, so the lines just before a dump can
    /// be missing. Both are bench-only limits; #619 gets a doorbell and a per-hart fatal state.
    fn check_alive(&self) {
        let c = C1_EXIT.load(core::sync::atomic::Ordering::Acquire);
        if c != 0 {
            crate::kcheck!(
                "core1.alive",
                lichen_kcore::check::Verdict::Fail,
                "core 1 exited with code {} (its dump is above)",
                c - 1
            );
            platform::exit(c);
        }
    }
}

/// Hart 1's Rust entry, from `_start1` (own stack, mtvec = trap_entry, mscratch = its trap stack top).
#[no_mangle]
extern "C" fn kmain1() -> ! {
    let mtvec = hw::trap::mtvec();
    kprintln!("core1: up, mhartid={} mtvec={mtvec:#010x}; its own PMP follows", hartid());
    crate::pmp::report();
    let cfg = core::array::from_fn::<u32, 4, _>(|w| {
        (0..4).fold(0u32, |acc, b| acc | (crate::pmp::cfg_byte(w * 4 + b) as u32) << (8 * b))
    });
    // SAFETY: hart 1 is the only consumer of TO_C1 and the only producer of TO_C0; kmain1 runs once, before anything
    // on core 1 calls `c1`.
    unsafe { C1.get() }.write(C1Ends { rx: unsafe { TO_C1.consumer() }, tx: unsafe { TO_C0.producer() } });
    let up = Msg { op: OP_UP, seq: 0, a: [hartid() as u32, mtvec, cfg[0], cfg[1], cfg[2], cfg[3]] };
    reply(up);
    loop {
        // SAFETY: core 1, after the write above; the borrow ends with this statement.
        let Some(m) = (unsafe { c1() }).rx.pop() else {
            core::hint::spin_loop();
            continue;
        };
        if m.op & REPLY != 0 {
            // An answer to a forwarded call whose stage already ended (#619): nobody waits for it.
            continue;
        }
        let a = match m.op {
            OP_PING => m.a.map(|x| !x),
            OP_COHERE => {
                COHERE_WORD.store(m.a[0], core::sync::atomic::Ordering::Relaxed);
                m.a
            }
            OP_TICK => tick_run(m.a[0] as u64, m.a[1]),
            OP_USPIN => u_spin(m.a[0] as usize),
            OP_URUN => u_run(m.a[0] as usize, m.a[1] as usize, m.a[2]),
            OP_PROCS => crate::procs::stage(m.a[0], m.a[1]),
            OP_BOOT => {
                [hw::trap::hwlp_state(), hw::trap::pie_state(), hw::trap::mcounteren(), hw::trap::mstatus(), 0, 0]
            }
            OP_PARK => {
                reply(Msg { op: m.op | REPLY, seq: m.seq, a: [0; 6] });
                park()
            }
            _ => [u32::MAX; 6],
        };
        reply(Msg { op: m.op | REPLY, seq: m.seq, a });
    }
}

/// Core 0 never has more requests outstanding than the reply queue holds, so a full queue only means core 0 is
/// slow or gone: retry.
fn reply(m: Msg) {
    // SAFETY: core 1's mailbox loop, outside any stage; the borrow ends in this call.
    let tx = unsafe { &mut c1().tx };
    while tx.push(m).is_err() {
        core::hint::spin_loop();
    }
}

fn park() -> ! {
    loop {
        hw::trap::wfi();
    }
}

/// `count` ticks of `period_us` on this hart's CLINT + CLIC, taken in mtrap's wait window as on core 0.
fn tick_run(period_us: u64, count: u32) -> [u32; 6] {
    let period = crate::timebase::us_to_ticks(period_us);
    let stray0 = mtrap::stray_irqs();
    mtrap::tick_start(period, count);
    let deadline = hw::clint::mtime() + period * (count as u64 + 1000);
    while mtrap::tick_count() < count && hw::clint::mtime() < deadline {
        mtrap::wait_irq();
    }
    let Some(t) = mtrap::tick_stop() else { return [0; 6] };
    let s = t.stats;
    let stray = mtrap::stray_irqs() - stray0;
    [s.count, s.gap_min as u32, s.gap_max as u32, s.lat_max as u32, s.missed as u32, stray]
}

/// #609 row 3 on core 1: a U spin under core 1's own PMP view is preempted by core 1's tick, and the illegal
/// instruction at its end still arrives.
fn u_spin(spin: usize) -> [u32; 6] {
    let udata = u_view();
    mtrap::tick_start(crate::timebase::us_to_ticks(U_TICK_US), u32::MAX);
    let u = mtrap::run_u(u_spin_illegal as *const () as usize, udata + UPAGE, spin, 0);
    let ticks = mtrap::tick_stop().map_or(0, |k| k.stats.count);
    let (code, epc) = u.fault.map_or((0xFFF, 0), |f| (lichen_kcore::trap::decode(f.cause).code as u32, f.epc));
    [u.preempts, code, epc, ticks, u.exit.unwrap_or(0) as u32, u.exit.is_some() as u32]
}

/// Load core 1's U view into its own PMP through the planner and `pmp::load` (so `view_ok` holds): the stub text
/// (R+X), its data page (R+W), and the fixed entries every U view carries, the R-only CPU-subsystem window (entry 14,
/// #646: a PMP-denied U load there hangs the core, an allowed one is refused by the hardware check) and deny-all.
/// Core 1's data and stack page is the first page of app slot 1's data region (kcore slots), so core 0's U runs
/// (slot 0) and core 1's never share a page. Returns the data page.
fn u_view() -> usize {
    // SAFETY: a linker symbol; only its address is used.
    let utext = unsafe { &_utext as *const u8 as usize };
    let udata = platform::slot(1).data;
    crate::pmp::apply(utext, udata, UPAGE);
    udata
}

/// Run one U stub on core 1 (the core-local deny rows and the access shapes, review H1 on both harts). The entry must
/// lie on the U stub page, so a bad message can never make core 1 run U code anywhere else.
fn u_run(entry: usize, a0: usize, limit_ms: u32) -> [u32; 6] {
    // SAFETY: a linker symbol; only its address is used.
    let utext = unsafe { &_utext as *const u8 as usize };
    if !(utext..utext + UPAGE).contains(&entry) {
        return [u32::MAX; 6];
    }
    let udata = u_view();
    let u = match limit_ms {
        0 => mtrap::run_u(entry, udata + UPAGE, a0, 0),
        ms => mtrap::run_u_limit(entry, udata + UPAGE, a0, 0, ms as u64 * 1000),
    };
    let (code, tval, epc) =
        u.fault.map_or((0xFFF, 0, 0), |f| (lichen_kcore::trap::decode(f.cause).code as u32, f.tval, f.epc));
    let mil = hw::trap::mintstatus() >> 24;
    [code, tval, epc, u.exit.unwrap_or(0) as u32, u.exit.is_some() as u32, u.timed_out as u32 | mil << 8]
}
