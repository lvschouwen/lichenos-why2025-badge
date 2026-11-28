//! #617 bench rows, run by core 0 before core 1 parks: the process stage on core 1 (`procs`), judged from core 1's
//! process table once it replied, the switch cost PERF line, and a ping afterwards as proof core 1's kernel survived.
//! A second stage proves the limits the first one never reaches: its deadline (idle never finishes) and a budget (a
//! spinner that never faults). Before the stages, the run_u limit on both cores (review-617 addendum).
use crate::bringup::{begin, record};
use crate::platform::{self, hw};
use crate::procs::{self, APPS, BUDGET, IDLE, NEVER, TARGET_OFFSET, UNTIL};
use crate::sched::{self, End, Kernel, State};
use crate::smp::{self, Link, REPLY};
use crate::{mtrap, timebase};
use lichen_kcore::check::Tally;
use lichen_kcore::perf::{self, Us100};

/// OP_PROCS reply deadline: the stage's own deadline is 1 s.
const STAGE_MS: u64 = 2000;
/// Tick-driven switches each way that count as alternating (the spinners overlap for about 30 ticks).
const MIN_SWITCHES: u32 = 5;
const LOAD_FAULT: usize = 5;
const STORE_FAULT: usize = 7;
/// The second stage: P1 never faults and ends on this budget; the stage ends on this deadline (idle never finishes).
const BUDGET2: u32 = 20;
const DEADLINE2_MS: u32 = 80;
/// The short run_u limit of the limit rows.
const U_LIMIT_MS: u32 = 50;
const UPAGE: usize = 4096;
/// The no-fault code of an OP_URUN reply.
const NO_FAULT: u32 = 0xFFF;

extern "C" {
    fn u_idle();
    static _utext: u8;
}

pub fn run(t: &mut Tally, l: &mut Link, cpu_hz: u64) {
    u_wfi(t, l);
    u_limit_core1(t, l);
    u_limit_finite_tick(t);
    // Stage 1: alternation, the two faults, idle, the switch cost.
    if !load(t, "proc.load", UNTIL, [BUDGET; APPS]) {
        return;
    }
    let [outcome, ticks, mintstatus, at_leave, stack, shadow] = stage(t, l, "proc.run", 0, 0);
    // SAFETY: core 1 replied, so its stage is over and it does not touch its table again (the mailbox pop ordered
    // its writes before this); this is the only reference to it on core 0.
    let k = unsafe { sched::of(1) };
    record(
        t,
        "proc.run",
        Some(outcome == procs::DONE && mintstatus == 0 && shadow == 3),
        format_args!(
            "core 1 ran the stage for {ticks} ticks of its own 1 ms tick: outcome {outcome} (0 done, 1 deadline, \
             2 nothing runnable, 3 not prepared); mintstatus {at_leave:#x} in the last trap, {mintstatus:#x} after \
             (want 0); {stack} B of core 1's stack used below the stage's frame; PMP = shadow at start/end {}/{}",
            shadow & 1,
            shadow >> 1
        ),
    );
    alternation(t, k);
    for i in 0..APPS {
        fault_row(t, k, i, ["proc.fault.p0", "proc.fault.p1"][i]);
    }
    let idle = &k.procs[IDLE];
    record(
        t,
        "proc.idle",
        Some(idle.state == State::Ready && idle.ticks >= procs::IDLE_TICKS),
        format_args!(
            "idle ran {} tick(s) (want >= {}), state {:?}, end {:x?}",
            idle.ticks,
            procs::IDLE_TICKS,
            idle.state,
            idle.end
        ),
    );
    // SAFETY: as `k` above: core 1 is done with its samples.
    perf_line(unsafe { sched::perf_of(1) }, cpu_hz);
    alive(t, l, "proc.alive");
    stage2(t, l);
}

/// Stage 2: P0 as before, P1 never faults (budget BUDGET2), idle never finishes, so the deadline ends the stage.
fn stage2(t: &mut Tally, l: &mut Link) {
    if !load(t, "proc.load2", [UNTIL[0], NEVER], [BUDGET, BUDGET2]) {
        return;
    }
    let [outcome, ticks, mintstatus, _, _, shadow] = stage(t, l, "proc.deadline", DEADLINE2_MS, u32::MAX);
    record(
        t,
        "proc.deadline",
        Some(outcome == procs::TIMED_OUT && mintstatus == 0 && shadow == 3),
        format_args!(
            "stage with a {DEADLINE2_MS} ms deadline and idle never done: outcome {outcome} (want 1, deadline) after \
             {ticks} ticks, mintstatus {mintstatus:#x} after (want 0), PMP = shadow at start/end {}/{}",
            shadow & 1,
            shadow >> 1
        ),
    );
    // SAFETY: as in `run`: core 1 replied.
    let k = unsafe { sched::of(1) };
    let p1 = &k.procs[1];
    record(
        t,
        "proc.budget",
        Some(p1.end == Some(End::Budget) && p1.ticks == BUDGET2 + 1),
        format_args!(
            "P1 never faults: ended {:x?} after {} of its own ticks (want Budget after {})",
            p1.end,
            p1.ticks,
            BUDGET2 + 1
        ),
    );
    fault_row(t, k, 0, "proc.budget.p0");
    alive(t, l, "proc.alive2");
}

/// Core 0 fills core 1's table for a stage (`procs::prepare`).
fn load(t: &mut Tally, id: &str, until: [usize; APPS], budget: [u32; APPS]) -> bool {
    begin(id);
    let loaded = procs::prepare(until, budget);
    record(
        t,
        id,
        Some(loaded.is_ok()),
        format_args!(
            "P0 and P1 (spinners, until tick {until:?}, budget {budget:?}) in slots 0 and 1, idle on the U stub page, \
             into core 1's table: {loaded:?}"
        ),
    );
    loaded.is_ok()
}

/// Run one stage on core 1 and return its reply. No reply: core 1 may still run P0/P1 in slots 0 and 1, which the
/// harness reloads next (review M1): stall it and end the run here, fail closed.
fn stage(t: &mut Tally, l: &mut Link, id: &str, deadline_ms: u32, idle_ticks: u32) -> [u32; 6] {
    begin(id);
    let r = l.call(smp::OP_PROCS, [deadline_ms, idle_ticks, 0, 0, 0, 0], STAGE_MS);
    if let Some(r) = r.filter(|r| r.op == smp::OP_PROCS | REPLY) {
        return r.a;
    }
    // SAFETY: core 1 was started; nothing waits on it after this (the run ends below).
    let stalled = unsafe { hw::core1::stall(1_000_000) };
    record(
        t,
        id,
        Some(false),
        format_args!("no reply from core 1 within {STAGE_MS} ms; core 1 stalled: {stalled}; run ends here"),
    );
    crate::bringup::finish(*t)
}

fn alive(t: &mut Tally, l: &mut Link, id: &str) {
    begin(id);
    let a = [0x617, 1, 2, 3, 4, 5];
    let p = l.call(smp::OP_PING, a, 100);
    record(
        t,
        id,
        Some(p.is_some_and(|p| p.a == a.map(|x| !x))),
        format_args!("core 1 answers a ping: {}", if p.is_some() { "yes" } else { "NO" }),
    );
}

/// run_u's limit on core 1 (review L-b): `u_idle` spins for ever, only its 10 ms watchdog tick preempts it, and the
/// U_LIMIT_MS limit ends the run on a tick, through to_kernel's mret. Then the CLIC level is 0 and core 1 answers.
fn u_limit_core1(t: &mut Tally, l: &mut Link) {
    begin("proc.u-limit");
    let r = l.call(smp::OP_URUN, [u_idle as *const () as usize as u32, 0, U_LIMIT_MS, 0, 0, 0], 500);
    let Some([code, _, _, _, exited, flags]) = r.map(|r| r.a) else {
        record(t, "proc.u-limit", Some(false), format_args!("no reply within 500 ms"));
        return;
    };
    let (timed_out, mil) = (flags & 1, flags >> 8);
    let a = [0x657, 0, 0, 0, 0, 0];
    let ping = l.call(smp::OP_PING, a, 100).is_some_and(|p| p.a == a.map(|x| !x));
    record(
        t,
        "proc.u-limit",
        Some(timed_out == 1 && code == NO_FAULT && exited == 0 && mil == 0 && ping),
        format_args!(
            "U spin on core 1 with a {U_LIMIT_MS} ms run limit: timed out {timed_out}, fault code {code:#x} (want none), \
             exited {exited}, mintstatus.mil after {mil:#x} (want 0), ping after: {ping}"
        ),
    );
}

/// Review L3 on core 0: a finite tick (5 x 100 us) runs out during a U spin; the compare must go to the run's
/// deadline instead of disarming, or the run never returns (the BEGIN line names the hang).
fn u_limit_finite_tick(t: &mut Tally) {
    begin("u.limit-tick");
    // SAFETY: a linker symbol; only its address is used.
    let utext = unsafe { &_utext as *const u8 as usize };
    let udata = platform::slot(0).data;
    crate::pmp::apply(utext, udata, UPAGE);
    let t0 = hw::clint::mtime();
    mtrap::tick_start(timebase::us_to_ticks(100), 5);
    let u = mtrap::run_u_limit(u_idle as *const () as usize, udata + UPAGE, 0, 0, U_LIMIT_MS as u64 * 1000);
    let tick = mtrap::tick_stop().map_or(0, |k| k.stats.count);
    let us = timebase::ticks_to_us(hw::clint::mtime() - t0);
    let mil = hw::trap::mintstatus() >> 24;
    record(
        t,
        "u.limit-tick",
        Some(u.timed_out && u.fault.is_none() && u.exit.is_none() && tick == 5 && mil == 0),
        format_args!(
            "U spin with a 5-tick tick and a {U_LIMIT_MS} ms limit: timed out {}, {tick} tick(s) (want 5), returned \
             after {us} us, mintstatus.mil {mil:#x} (want 0)",
            u.timed_out
        ),
    );
}

/// The U `wfi` policy (#657): a U wfi on core 1 is an illegal instruction and ends the process (the idle process
/// spins, see `procs`). run_u always has a tick, so the stub cannot park the hart if that ever changes; it exits
/// with a0 if the wfi returns.
fn u_wfi(t: &mut Tally, l: &mut Link) {
    extern "C" {
        fn u_wfi();
    }
    begin("proc.u-wfi");
    let r = l.call(smp::OP_URUN, [u_wfi as *const () as usize as u32, 0x617, 0, 0, 0, 0], 100);
    let Some([code, tval, epc, a0, exited, _]) = r.map(|r| r.a) else {
        record(t, "proc.u-wfi", Some(false), format_args!("no reply within 100 ms"));
        return;
    };
    // U wfi policy (#657): illegal, the process ends. TW is WARL and reads back 0 here, yet the core refuses U wfi.
    record(
        t,
        "proc.u-wfi",
        Some(code == 2 && exited == 0 && epc == u_wfi as *const () as usize as u32),
        format_args!(
            "U wfi on core 1: exited {exited} (a0 {a0:#x}), else code {code} ({}) mtval {tval:#x} mepc {epc:#x}",
            lichen_kcore::trap::exception_name(code as u16)
        ),
    );
}

/// A spinner's own round counter, at the start of its data.
fn rounds(i: usize) -> u32 {
    // SAFETY: an aligned word in app slot i's data (L2MEM), written by core 1's spinner through the shared L1 DCache.
    unsafe { core::ptr::read_volatile(platform::slot(i).data as *const u32) }
}

fn alternation(t: &mut Tally, k: &Kernel) {
    let (ab, ba) = (k.preempts[0][1], k.preempts[1][0]);
    let (r0, r1) = (rounds(0), rounds(1));
    record(
        t,
        "proc.alternate",
        Some(ab >= MIN_SWITCHES && ba >= MIN_SWITCHES && r0 > 0 && r1 > 0),
        format_args!(
            "tick-driven switches P0->P1 {ab}, P1->P0 {ba} (want >= {MIN_SWITCHES} each; the spinners never yield); \
             rounds P0 {r0}, P1 {r1}; ticks P0 {}, P1 {}",
            k.procs[0].ticks, k.procs[1].ticks
        ),
    );
}

/// Spinner i ended on a fault at the other one's data, from its own text; P1's store left P0's word unchanged.
fn fault_row(t: &mut Tally, k: &Kernel, i: usize, id: &str) {
    let p = &k.procs[i];
    let want_cause = if i == 0 { LOAD_FAULT } else { STORE_FAULT };
    let target = procs::target(i);
    let in_text = |pc: usize| pc >= p.slot.text && pc < p.slot.text + p.slot.len;
    let ok =
        matches!(p.end, Some(End::Fault { cause, tval, pc }) if cause == want_cause && tval == target && in_text(pc));
    // SAFETY: an aligned word in the other slot's data (L2MEM); nothing runs there now.
    let word = unsafe { core::ptr::read_volatile(target as *const u32) };
    record(
        t,
        id,
        Some(ok && p.state == State::Ended && word == 0),
        format_args!(
            "{} ended {:x?} (want cause {want_cause} at {target:#x}, the other slot's data + {TARGET_OFFSET:#x}, pc in \
             its own text); the word there reads {word:#x} (want 0)",
            p.name, p.end
        ),
    );
}

/// The switch cost: mcycle on core 1 from the trap entry stamp to the end of trap_handler, for tick-driven switches.
fn perf_line(p: &mut sched::SwitchPerf, hz: u64) {
    let sw = &mut p.switch;
    let n = sw.n;
    let s = &mut sw.buf[..n];
    let (lo, hi) = (s.iter().copied().min().unwrap_or(0), s.iter().copied().max().unwrap_or(0));
    let (med, p95) = perf::median_p95(s).unwrap_or((0, 0));
    let pm = &mut p.pmp;
    let (pmed, pp95) = perf::median_p95(&mut pm.buf[..pm.n]).unwrap_or((0, 0));
    let us = |c: u32| Us100(perf::cycles_to_us100(c, hz));
    crate::kprintln!("perf: kernel.ctx_switch = core 1 tick switch, trap entry stamp to trap_handler end (no restore)");
    crate::kprintln!(
        "PERF kernel.ctx_switch median_us={} p95_us={} min_us={} max_us={} n={n} median_cycles={med} \
         pmp_median_cycles={pmed} pmp_p95_cycles={pp95} pmp_median_us={} pmp_writes={} cpu_hz={hz} mtime_hz={}",
        us(med),
        us(p95),
        us(lo),
        us(hi),
        us(pmed),
        p.pmp_writes,
        crate::timebase::hz()
    );
    let [m0, m1, m2, m3] = p.marks;
    crate::kprintln!(
        "perf: kernel.ctx_switch last switch, cycles: on_trap {m0}, tick {m1}, supervisor {m2}, scheduled {m3}"
    );
}
