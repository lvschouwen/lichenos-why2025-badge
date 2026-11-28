//! #618 bench rows, run by core 0 after the #617 stages: two apps built as their own crates run on core 1 (`procs_apps`),
//! and a misbehaving one is ended while the other keeps running. Three stages:
//!   crash   rogue stores into the worker's memory: it faults and only it ends
//!   spin    rogue never faults: its cycle budget ends it
//!   replace core 0 loads the rogue (exit 77) over the worker's image in slot 0, which core 1 ran just before; the
//!           exit code says whose bytes core 1's instruction fetch saw
//! After each kill the rows check what termination gave back (memory zeroed, handles, PMP view) and print the per
//! process CPU accounting. The numbers are core 1's own mcycle, so they hold for whatever clock core 1 runs at.
use crate::bringup::{begin, record};
use crate::bringup_procs::{alive, stage};
use crate::platform;
use crate::procs::{self, IDLE};
use crate::procs_apps::{self as apps, ROGUE_SLOT, WORKER_SLOT};
use crate::sched::{self, End, Kernel, State};
use lichen_kcore::check::Tally;

/// Both kill stages end on this deadline (idle never finishes), well inside OP_PROCS' reply wait.
const DEADLINE_MS: u32 = 150;
/// The worker must have run this many ticks after the rogue ended, alone.
const SURVIVE_TICKS: u64 = 50;
/// The spinner's CPU budget: 10 ms of cycles, so it ends after about 20 ms of wall time (the worker takes every other
/// tick), and well before the deadline.
const SPIN_BUDGET_MS: u64 = 10;
/// A budget kill happens on the first trap past the budget: at most this many ticks of cycles late.
const KILL_SLACK_TICKS: u64 = 3;
/// How far (percent) the meters plus the kernel's trap time may sit from the stage's clock: the first process's span
/// starts a few hundred cycles before the stage's clock does, and the last trap's kernel time is not charged.
const TOLERANCE_PCT: u64 = 1;

pub fn run(t: &mut Tally, l: &mut crate::smp::Link, cpu_hz: u64) {
    let budget = cpu_hz / 1000 * SPIN_BUDGET_MS;
    if !load(t, "apps.crash.load", || apps::prepare_pair(apps::CRASH, None)) {
        return;
    }
    stage(t, l, "apps.crash", DEADLINE_MS, u32::MAX);
    // SAFETY: core 1 replied, so its stage is over and it does not touch its table again; the only reference on core 0.
    let k = unsafe { sched::of(1) };
    crash_rows(t, k);
    released(t, k, "apps.crash.released");
    accounting(t, k, "apps.crash", cpu_hz);
    alive(t, l, "apps.alive");

    if !load(t, "apps.spin.load", || apps::prepare_pair(apps::SPIN, Some(budget))) {
        return;
    }
    stage(t, l, "apps.spin", DEADLINE_MS, u32::MAX);
    // SAFETY: as above.
    let k = unsafe { sched::of(1) };
    spin_rows(t, k, budget, cpu_hz);
    released(t, k, "apps.spin.released");
    accounting(t, k, "apps.spin", cpu_hz);

    if !load(t, "apps.replace.load", apps::prepare_replace) {
        return;
    }
    let outcome = stage(t, l, "apps.replace", 0, 0)[0];
    // SAFETY: as above.
    let k = unsafe { sched::of(1) };
    replace_row(t, k, outcome);
    alive(t, l, "apps.alive2");
}

fn load(t: &mut Tally, id: &str, f: impl FnOnce() -> Result<(), crate::loader::LoadError>) -> bool {
    begin(id);
    let r = f();
    record(t, id, Some(r.is_ok()), format_args!("worker and rogue (own crates) into core 1's table: {r:?}"));
    r.is_ok()
}

/// A word range reads all zero (core 0 reads what core 1 zeroed through the shared L1 DCache).
fn zeroed(addr: usize, len: usize) -> bool {
    // SAFETY: aligned words inside an app slot (NOLOAD L2MEM, reserved for apps); nothing runs there now.
    (addr..addr + len).step_by(4).all(|a| unsafe { core::ptr::read_volatile(a as *const u32) } == 0)
}

/// The worker is still running after the rogue ended: it ran alone for SURVIVE_TICKS, its counter moved.
fn survivor_ok(k: &Kernel) -> (bool, u64, u32) {
    let (w, r) = (&k.procs[WORKER_SLOT], &k.procs[ROGUE_SLOT]);
    let alone = k.ticks.saturating_sub(r.ended_tick);
    // SAFETY: the worker's first data word is its round counter (apps/worker); aligned L2MEM, written by core 1 only.
    let rounds = unsafe { core::ptr::read_volatile(platform::slot(WORKER_SLOT).data as *const u32) };
    (w.state == State::Ready && w.end.is_none() && alone >= SURVIVE_TICKS && rounds > 0, alone, rounds)
}

fn crash_rows(t: &mut Tally, k: &Kernel) {
    let r = &k.procs[ROGUE_SLOT];
    let text = platform::slot(ROGUE_SLOT).text;
    let fault = matches!(r.end, Some(End::Fault { cause: 7, tval, pc })
        if tval == apps::target() && pc >= text && pc < text + r.slot.len);
    let (alive, alone, rounds) = survivor_ok(k);
    record(
        t,
        "apps.crash.kill",
        Some(fault && r.state == State::Ended && alive),
        format_args!(
            "rogue ended {:x?} (want a store fault, cause 7, at {:#x} in the worker's data, pc in its own text); the \
             worker ran on alone for {alone} ticks (want >= {SURVIVE_TICKS}), {rounds} rounds, state {:?}, end {:x?}",
            r.end,
            apps::target(),
            k.procs[WORKER_SLOT].state,
            k.procs[WORKER_SLOT].end
        ),
    );
}

fn spin_rows(t: &mut Tally, k: &Kernel, budget: u64, cpu_hz: u64) {
    let r = &k.procs[ROGUE_SLOT];
    let slack = KILL_SLACK_TICKS * cpu_hz / 1000;
    let cycles = r.meter.cycles();
    let (alive, alone, rounds) = survivor_ok(k);
    record(
        t,
        "apps.spin.kill",
        Some(r.end == Some(End::Budget) && cycles > budget && cycles <= budget + slack && alive),
        format_args!(
            "rogue spins: ended {:x?} (want Budget) at {cycles} cycles of a {budget} budget (want over it, at most {slack} \
             over); the worker ran on alone for {alone} ticks, {rounds} rounds",
            r.end
        ),
    );
}

/// What termination gave back: the rogue's slot memory is zero, its handles are gone, its PMP view is dropped; the
/// worker keeps its own handle and its memory (it would have ended on a changed canary or a dead handle).
fn released(t: &mut Tally, k: &Kernel, id: &str) {
    let r = &k.procs[ROGUE_SLOT];
    let s = platform::slot(ROGUE_SLOT);
    let mem = zeroed(s.data, s.len) && zeroed(s.text, r.image_len.next_multiple_of(4));
    let handles = k.handles.first_of(ROGUE_SLOT).is_none() && k.handles.first_of(WORKER_SLOT).is_some();
    record(
        t,
        id,
        Some(mem && handles && r.view.is_none() && k.procs[WORKER_SLOT].view.is_some()),
        format_args!(
            "rogue's text image ({} B) and data region ({} B) zero: {mem}; its handles gone and the worker's kept: \
             {handles}; PMP view dropped: {}; reclaim took {} cycles",
            r.image_len,
            s.len,
            r.view.is_none(),
            r.reclaim_cycles
        ),
    );
}

/// Per-process CPU accounting: PERF lines for the numbers, one row for how well they add up to the stage's clock.
fn accounting(t: &mut Tally, k: &Kernel, id: &str, cpu_hz: u64) {
    let mut sum = 0u64;
    for p in &k.procs {
        sum += p.meter.cycles();
        crate::kprintln!(
            "PERF proc.account stage={id} name={} cycles={} ticks={} syscalls={} reclaim_cycles={} cpu_hz={cpu_hz}",
            if p.name.is_empty() { "-" } else { p.name },
            p.meter.cycles(),
            p.ticks,
            p.syscalls,
            p.reclaim_cycles
        );
    }
    let wall = k.stage_cycles as u64;
    let kernel = k.kernel_time.cycles();
    crate::kprintln!("PERF proc.account stage={id} name=kernel cycles={kernel} cpu_hz={cpu_hz}");
    let id = account_id(id);
    let all = sum + kernel;
    record(
        t,
        id.as_str(),
        Some(all.abs_diff(wall) * 100 <= wall * TOLERANCE_PCT && k.procs[IDLE].meter.cycles() == 0),
        format_args!(
            "the processes ({sum}) and the kernel's own trap time ({kernel}) add up to {all} of the stage's {wall} \
             cycles (want within {TOLERANCE_PCT}%); the idle process never ran: {}",
            k.procs[IDLE].meter.cycles() == 0
        ),
    );
}

/// `<stage>.account` as a row id.
fn account_id(stage: &str) -> lichen_abi::Line {
    use core::fmt::Write;
    let mut l = lichen_abi::Line::new();
    let _ = write!(l, "{stage}.account");
    l
}

fn replace_row(t: &mut Tally, k: &Kernel, outcome: u32) {
    let p = &k.procs[WORKER_SLOT];
    record(
        t,
        "apps.replace",
        Some(p.end == Some(End::Exit(apps::ROGUE_EXIT_CODE)) && outcome == procs::DONE),
        format_args!(
            "core 1 ran the worker from slot 0, core 0 loaded the rogue over it, core 1 ran: ended {:x?} (want Exit({}); \
             the worker would never exit), stage outcome {outcome} (want 0)",
            p.end,
            apps::ROGUE_EXIT_CODE
        ),
    );
}
