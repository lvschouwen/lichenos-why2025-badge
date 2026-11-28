//! #619 bench rows, run by core 0 inside the #618 app stages: syscall forwarding from core 1 to core 0.
//!   latency  the rogue alone makes PINGS one-byte WRITEs; each one goes to core 0's console and back. Nothing else
//!            can run, so core 1 waits for each reply in the trap: PERF kernel.syscall.xcore is that round trip.
//!   flood    the rogue floods full-size WRITEs for the whole stage next to the worker. Each call blocks it, core 0
//!            holds the replies while its console is short of room, and the worker must keep at least FLOOR_PCT of
//!            core 1's cycles (its fair share of two is 50 %; the floor leaves the kernel's trap time out).
//!   ping     the same flood, the worker also a pinger (a short WRITE every PING_EVERY rounds): held WRITEs are
//!            served in arrival order, so each ping waits behind at most one flood line, and its round trip stays
//!            under PING_BOUND_MS (about 4.3 ms of console drain for one 267 B line at 66 B/ms, plus up to two ticks
//!            for the reply to be picked up and the worker's turn).
//!   budget   the same flood with a small cycle budget: the forwarding work billed to the rogue ends it on its budget,
//!            with a call in flight; its late reply is stale on core 1, and termination gives everything back.
use crate::bringup::{begin, record};
use crate::bringup_apps::{accounting, released, survivor_ok, KILL_SLACK_TICKS};
use crate::bringup_procs::{alive, stage};
use crate::procs;
use crate::procs_apps::{self as apps, ROGUE_SLOT, WORKER_SLOT};
use crate::sched::{self, End, Kernel, Samples, State};
use crate::smp::Link;
use crate::{forward, platform, services};
use lichen_kcore::check::Tally;
use lichen_kcore::perf::{self, Us100};

/// The latency stage ends once the rogue exited and idle ran a tick; this deadline only bounds a hang.
const LATENCY_DEADLINE_MS: u32 = 150;
/// The flood runs this long (it never ends by itself).
const FLOOD_MS: u32 = 150;
/// The worker's share of core 1's cycles in the flood must stay at or above this.
const FLOOR_PCT: u64 = 45;

/// The budget stage's rogue: well under the ~50k cycles its flood takes over FLOOD_MS (50059 on the c9152874 bench).
const FLOOD_BUDGET: u64 = 20_000;

/// The pinger's period in worker rounds (a round is about 30 us at 40 MHz alone), its round-trip bound, and how many
/// pings the stage must see come back.
const PING_EVERY: usize = 200;
const PING_BOUND_MS: u64 = 10;
const MIN_PINGS: u32 = 5;

pub fn run(t: &mut Tally, l: &mut Link, cpu_hz: u64) {
    latency(t, l, cpu_hz);
    flood(t, l, cpu_hz);
    ping(t, l, cpu_hz);
    budget(t, l, cpu_hz);
}

fn ping(t: &mut Tally, l: &mut Link, cpu_hz: u64) {
    if !load(t, "fwd.ping.load", || apps::prepare_pair_ping(PING_EVERY, apps::WRITE_LOOP, None)) {
        return;
    }
    let outcome = stage(t, l, "fwd.ping", FLOOD_MS, u32::MAX)[0];
    let svc = services::take_stats();
    // SAFETY: as in `latency`.
    let k = unsafe { sched::of(1) };
    let (w, r) = (&k.procs[WORKER_SLOT], &k.procs[ROGUE_SLOT]);
    let max_us = perf::cycles_to_us100(w.call_max, cpu_hz) / 100;
    let bound_cycles = cpu_hz / 1000 * PING_BOUND_MS;
    record(
        t,
        "fwd.ping.bound",
        Some(
            outcome == procs::TIMED_OUT
                && w.state.alive()
                && w.end.is_none()
                && r.end.is_none()
                && w.call_n >= MIN_PINGS
                && (w.call_max as u64) <= bound_cycles,
        ),
        format_args!(
            "the worker pinged during the flood: {} pings came back (want >= {MIN_PINGS}), the slowest in {max_us} us \
             (want <= {PING_BOUND_MS} ms); worker {:?} end {:x?}, rogue {:?} end {:x?} with {} of its WRITEs back; core 0 \
             served {}, held {} for room, refused {}",
            w.call_n,
            w.state,
            w.end,
            r.state,
            r.end,
            r.call_n,
            svc.served,
            svc.held,
            svc.refused
        ),
    );
    crate::kprintln!(
        "PERF kernel.syscall.xcore_ping max_us={max_us} n={} bound_ms={PING_BOUND_MS} cpu_hz={cpu_hz}",
        w.call_n
    );
    alive(t, l, "fwd.alive4");
}

fn load(t: &mut Tally, id: &str, f: impl FnOnce() -> Result<(), crate::loader::LoadError>) -> bool {
    begin(id);
    let r = f();
    record(t, id, Some(r.is_ok()), format_args!("into core 1's table: {r:?}"));
    // SAFETY: core 1 waits in its mailbox loop (it has not got OP_PROCS yet), so its samples are core 0's now.
    let p = unsafe { forward::perf() };
    p.fast.n = 0;
    p.slow.n = 0;
    services::take_stats();
    r.is_ok()
}

/// The rogue's complete WRITEs, the first word of its data (apps/rogue).
fn writes(slot: usize) -> u32 {
    // SAFETY: an aligned word in an app slot's data (L2MEM), written by core 1's rogue; nothing runs there now.
    unsafe { core::ptr::read_volatile(platform::slot(slot).data as *const u32) }
}

fn latency(t: &mut Tally, l: &mut Link, cpu_hz: u64) {
    if !load(t, "fwd.latency.load", || apps::prepare_solo(apps::PING)) {
        return;
    }
    let outcome = stage(t, l, "fwd.latency", LATENCY_DEADLINE_MS, 1)[0];
    let svc = services::take_stats();
    // SAFETY: core 1 replied, so its stage is over; the only references on core 0.
    let (k, p) = unsafe { (sched::of(1), forward::perf()) };
    let r = &k.procs[WORKER_SLOT];
    let calls = apps::PINGS + 1;
    record(
        t,
        "fwd.latency",
        Some(
            outcome == procs::DONE
                && r.end == Some(End::Exit(0))
                && k.fwd.sent == calls
                && svc.served == calls
                && k.calls.stale == 0,
        ),
        format_args!(
            "the rogue alone made {calls} forwarded WRITEs, each checked: ended {:x?} (want Exit(0); 5 = refused, 6 = \
             short); core 1 sent {}, answered in the trap {}, later {}, refused {}, stale {}; core 0 served {} \
             ({} B, {} held for room); stage outcome {outcome}",
            r.end, k.fwd.sent, k.fwd.fast, k.fwd.slow, k.fwd.refused, k.calls.stale, svc.served, svc.bytes, svc.held
        ),
    );
    perf_line("kernel.syscall.xcore", &mut p.fast, cpu_hz);
    alive(t, l, "fwd.alive");
}

fn flood(t: &mut Tally, l: &mut Link, cpu_hz: u64) {
    if !load(t, "fwd.flood.load", || apps::prepare_pair(apps::WRITE_LOOP, None)) {
        return;
    }
    let outcome = stage(t, l, "fwd.flood", FLOOD_MS, u32::MAX)[0];
    let svc = services::take_stats();
    // SAFETY: as in `latency`.
    let (k, p) = unsafe { (sched::of(1), forward::perf()) };
    fair_row(t, k, outcome);
    let r = &k.procs[ROGUE_SLOT];
    let w = writes(ROGUE_SLOT);
    record(
        t,
        "fwd.flood.served",
        Some(r.state.alive() && r.end.is_none() && w > 0 && svc.refused == 0 && k.fwd.refused == 0),
        format_args!(
            "the flooding rogue is still alive ({:?}, end {:x?}) and {w} of its full WRITEs came back complete; core 1 \
             sent {}, refused {}, stale {}; core 0 served {} ({} B), held {} for room, dropped {} at the stage end, \
             refused {}; the log ring kept its {} B reserve (the log.ring row counts drops)",
            r.state,
            r.end,
            k.fwd.sent,
            k.fwd.refused,
            k.calls.stale,
            svc.served,
            svc.bytes,
            svc.held,
            svc.dropped,
            svc.refused,
            services::RESERVE
        ),
    );
    accounting(t, k, "fwd.flood", cpu_hz);
    perf_line("kernel.syscall.xcore_contended", &mut p.slow, cpu_hz);
    alive(t, l, "fwd.alive2");
}

fn budget(t: &mut Tally, l: &mut Link, cpu_hz: u64) {
    if !load(t, "fwd.budget.load", || apps::prepare_pair(apps::WRITE_LOOP, Some(FLOOD_BUDGET))) {
        return;
    }
    stage(t, l, "fwd.budget", FLOOD_MS, u32::MAX);
    let svc = services::take_stats();
    // SAFETY: as in `latency`.
    let k = unsafe { sched::of(1) };
    let r = &k.procs[ROGUE_SLOT];
    let cycles = r.meter.cycles();
    let slack = KILL_SLACK_TICKS * cpu_hz / 1000;
    let (survived, alone, rounds) = survivor_ok(k);
    record(
        t,
        "fwd.budget.kill",
        Some(r.end == Some(End::Budget) && cycles > FLOOD_BUDGET && cycles <= FLOOD_BUDGET + slack && survived),
        format_args!(
            "the flooding rogue ended {:x?} (want Budget) at {cycles} cycles of a {FLOOD_BUDGET} budget (at most {slack} \
             over) after {} syscalls, {} forwarded; stale replies on core 1 {} (a call in flight at the kill answers \
             nobody); core 0 served {}, held {}, dropped {}; the worker ran on alone for {alone} ticks, {rounds} rounds",
            r.end,
            r.syscalls,
            k.fwd.sent,
            k.calls.stale,
            svc.served,
            svc.held,
            svc.dropped
        ),
    );
    released(t, k, "fwd.budget.released");
    alive(t, l, "fwd.alive3");
}

/// The worker kept its share of core 1 while the rogue flooded.
fn fair_row(t: &mut Tally, k: &Kernel, outcome: u32) {
    let w = &k.procs[WORKER_SLOT];
    let (cycles, wall) = (w.meter.cycles(), k.stage_cycles.max(1));
    let pct100 = cycles * 10_000 / wall;
    // SAFETY: the worker's first data word is its round counter (apps/worker); aligned L2MEM, written by core 1 only.
    let rounds = unsafe { core::ptr::read_volatile(platform::slot(WORKER_SLOT).data as *const u32) };
    record(
        t,
        "fwd.flood.fair",
        Some(
            outcome == procs::TIMED_OUT
                && w.state == State::Ready
                && w.end.is_none()
                && rounds > 0
                && cycles * 100 >= wall * FLOOR_PCT,
        ),
        format_args!(
            "the worker ran {cycles} of the stage's {wall} cycles ({}.{:02} %, floor {FLOOR_PCT} %), {rounds} rounds, \
             state {:?}, end {:x?}, while the rogue flooded; stage outcome {outcome} (want 1, the {FLOOD_MS} ms deadline)",
            pct100 / 100,
            pct100 % 100,
            w.state,
            w.end
        ),
    );
}

/// One PERF line for a set of round-trip samples (core 1 mcycle, trap entry stamp to the caller's resume).
fn perf_line(metric: &str, s: &mut Samples, hz: u64) {
    let n = s.n;
    let v = &mut s.buf[..n];
    let (lo, hi) = (v.iter().copied().min().unwrap_or(0), v.iter().copied().max().unwrap_or(0));
    let (med, p95) = perf::median_p95(v).unwrap_or((0, 0));
    let us = |c: u32| Us100(perf::cycles_to_us100(c, hz));
    crate::kprintln!(
        "PERF {metric} median_us={} p95_us={} min_us={} max_us={} n={n} median_cycles={med} cpu_hz={hz}",
        us(med),
        us(p95),
        us(lo),
        us(hi)
    );
}
