//! #617 bench rows, run by core 0 before core 1 parks: the process stage on core 1 (`procs`), judged from core 1's
//! process table once it replied, the switch cost PERF line, and a ping afterwards as proof core 1's kernel survived.
use crate::bringup::{begin, record};
use crate::platform::{self, hw};
use crate::procs::{self, APPS, IDLE, TARGET_OFFSET};
use crate::sched::{self, End, Kernel, State};
use crate::smp::{self, Link, REPLY};
use lichen_kcore::check::Tally;
use lichen_kcore::perf::{self, Us100};

/// OP_PROCS reply deadline: the stage's own deadline is 1 s.
const STAGE_MS: u64 = 2000;
/// Tick-driven switches each way that count as alternating (the spinners overlap for about 30 ticks).
const MIN_SWITCHES: u32 = 5;
const LOAD_FAULT: usize = 5;
const STORE_FAULT: usize = 7;

pub fn run(t: &mut Tally, l: &mut Link, cpu_hz: u64) {
    u_wfi(t, l);
    begin("proc.load");
    let loaded = procs::prepare();
    record(
        t,
        "proc.load",
        Some(loaded.is_ok()),
        format_args!("P0 and P1 (spinners) in slots 0 and 1, idle on the U stub page, into core 1's table: {loaded:?}"),
    );
    if loaded.is_err() {
        return;
    }
    begin("proc.run");
    let r = l.call(smp::OP_PROCS, [0; 6], STAGE_MS);
    let Some(r) = r.filter(|r| r.op == smp::OP_PROCS | REPLY) else {
        // Core 1 may still run P0/P1 in slots 0 and 1, which the harness reloads next (review M1): stop it and end the
        // run here, fail closed.
        // SAFETY: core 1 was started; nothing waits on it after this (the run ends below).
        let stalled = unsafe { hw::core1::stall(1_000_000) };
        record(
            t,
            "proc.run",
            Some(false),
            format_args!("no reply from core 1 within {STAGE_MS} ms; core 1 stalled: {stalled}; run ends here"),
        );
        crate::bringup::finish(*t)
    };
    let [outcome, ticks, mintstatus, at_leave, stack, _] = r.a;
    // SAFETY: core 1 replied, so its stage is over and it does not touch its table again (the mailbox pop ordered
    // its writes before this); this is the only reference to it on core 0.
    let k = unsafe { sched::of(1) };
    record(
        t,
        "proc.run",
        Some(outcome == procs::DONE && mintstatus == 0),
        format_args!(
            "core 1 ran the stage for {ticks} ticks of its own 1 ms tick: outcome {outcome} (0 done, 1 deadline, \
             2 nothing runnable, 3 not prepared); mintstatus {at_leave:#x} in the last trap, {mintstatus:#x} after \
             (want 0); {stack} B of core 1's stack used below the stage's frame"
        ),
    );
    alternation(t, k);
    for i in 0..APPS {
        fault_row(t, k, i);
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
    begin("proc.alive");
    let a = [0x617, 1, 2, 3, 4, 5];
    let p = l.call(smp::OP_PING, a, 100);
    record(
        t,
        "proc.alive",
        Some(p.is_some_and(|p| p.a == a.map(|x| !x))),
        format_args!("core 1 answers a ping after the stage: {}", if p.is_some() { "yes" } else { "NO" }),
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
fn fault_row(t: &mut Tally, k: &Kernel, i: usize) {
    let id = ["proc.fault.p0", "proc.fault.p1"][i];
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
