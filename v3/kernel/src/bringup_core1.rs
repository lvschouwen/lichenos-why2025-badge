//! #612 checkpoint 3 bench rows, run by core 0 after the core 0 rows: start core 1, 1000 mailbox pings with the
//! round-trip PERF line (kernel.mailbox_rtt), a burst, the shared-cache evidence, core 1's own tick with its jitter PERF line (kernel.timer_jitter), and core 1
//! timer preemption of a U spin (#609 row 3). Every wait on core 1 is bounded by an mtime deadline and announced
//! with BEGIN, so a silent core 1 gives a FAIL row and still an END.
use crate::platform::{self, hw};
use crate::smp::{self, Link, Msg, REPLY};
use crate::timebase::{self, us_to_ticks};
use crate::{kcheck, kprintln};
use lichen_kcore::check::{Tally, Verdict};
use lichen_kcore::perf::{self, Us, Us100};
use lichen_kcore::trap::EXC_ILLEGAL;

const PINGS: usize = 1000;
const BURST: u32 = 16;
/// Core 1's tick under test: 5000 ticks of 1 ms.
const C1_TICK_US: u64 = 1000;
const C1_TICKS: u32 = 5000;
/// Jitter target (#612): under 2 us.
const JITTER_TARGET_US: u64 = 2;
const U_SPIN: u32 = 4_000_000;
const UPAGE: usize = 4096;

/// Round-trip samples in mtime ticks (bss, not the stack).
static RTT: crate::global::Global<[u32; PINGS]> = crate::global::Global::new([0; PINGS]);

extern "C" {
    fn u_illegal_at();
}

fn record(t: &mut Tally, id: &str, ok: Option<bool>, detail: core::fmt::Arguments) {
    let v = match ok {
        Some(true) => Verdict::Pass,
        Some(false) => Verdict::Fail,
        None => Verdict::Info,
    };
    t.record(v);
    kcheck!(id, v, "{detail}");
}

struct RegsLine(hw::core1::Regs);
impl core::fmt::Display for RegsLine {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let r = self.0;
        write!(
            f,
            "clk_ctrl0={:#010x} rst_en0={:#010x} pmu_sw_stall={:#010x} corestalled_st={:#010x}",
            r.clk_ctrl0, r.rst_en0, r.pmu_sw_stall, r.corestalled_st
        )
    }
}

fn begin(id: &str) {
    kprintln!("BEGIN {id}");
    platform::flush();
}

fn after_ms(ms: u64) -> u64 {
    hw::clint::mtime() + us_to_ticks(ms * 1000)
}

pub fn run(t: &mut Tally) {
    clic_top_line(t);
    begin("core1.start");
    let before = hw::core1::regs();
    kprintln!("core1: before {}", RegsLine(before));
    begin("core1.up");
    let Some((mut l, polls)) = smp::start() else {
        record(t, "core1.start", Some(false), format_args!("core 1 stayed stalled"));
        return;
    };
    // Silent until UP: core 1 owns the console meanwhile.
    let up = l.recv(after_ms(2000));
    let after = hw::core1::regs();
    record(
        t,
        "core1.start",
        Some(hw::core1::running()),
        format_args!("unstalled after {polls} polls, clock on, out of reset; after {}", RegsLine(after)),
    );
    let Some(up) = up.filter(|m| m.op == smp::OP_UP) else {
        let op = up.map_or(0, |m| m.op);
        record(t, "core1.up", Some(false), format_args!("no UP from core 1 within 2 s (got op {op:#x})"));
        return;
    };
    let [hart, mtvec, c0, c1, c2, c3] = up.a;
    let own = hw::trap::mtvec();
    record(
        t,
        "core1.up",
        Some(hart == 1 && mtvec == own),
        format_args!("mhartid={hart} mtvec={mtvec:#010x} (core 0 {own:#010x}, mode {})", mtvec & 3),
    );
    let locked = [c0, c1, c2, c3].iter().map(|w| (0..4).filter(|b| w >> (8 * b + 7) & 1 != 0).count()).sum::<usize>();
    record(
        t,
        "core1.pmp",
        Some(locked == 0),
        format_args!(
            "pmpcfg0-3 {c0:#010x} {c1:#010x} {c2:#010x} {c3:#010x} as core 1 read them at UP (ROM state after its granularity probe), {locked} locked"
        ),
    );

    boot_state(t, &mut l);
    pings(t, &mut l);
    burst(t, &mut l);
    coherence(t, &mut l);
    timer(t, &mut l);
    u_preempt(t, &mut l);
    u_deny(t, &mut l);
    // The access shapes go last on core 1: they are the likeliest to find a new hang (review H1).
    shapes(t, &mut l);

    begin("core1.park");
    let p = l.call(smp::OP_PARK, [0; 6], 100);
    let stale = l.stale();
    record(
        t,
        "core1.park",
        Some(p.is_some() && stale == 0),
        format_args!("core 1 acknowledged and parks in wfi; {stale} stale repl(ies) dropped over the run"),
    );
    clic1_sweep(t);
}

/// Review M2 on core 1: `_start1` zeroes the U-visible CSRs of its own hart (HWLP and PIE state, mcounteren), and
/// mstatus.FS is off.
fn boot_state(t: &mut Tally, l: &mut Link) {
    begin("core1.boot");
    let Some(r) = l.call(smp::OP_BOOT, [0; 6], 100) else {
        record(t, "core1.boot", Some(false), format_args!("no reply within 100 ms"));
        return;
    };
    let [hwlp, pie, mcounteren, mstatus, _, _] = r.a;
    record(
        t,
        "core1.boot",
        Some(hwlp & 3 == 0 && pie & 3 == 0 && mcounteren == 0 && mstatus & 0x6000 == 0),
        format_args!(
            "core 1: HWLP state {hwlp:#x}, PIE state {pie:#x} (low 2 bits 0 = off), mcounteren {mcounteren:#x}, \
             mstatus {mstatus:#010x} FS {} (want all 0)",
            (mstatus >> 13) & 3
        ),
    );
}

/// Run a U stub on core 1 with a0 = `arg`; None on a timeout. Reply a = [code, mtval, epc, exit a0, exited, 0].
fn u_run(l: &mut Link, f: unsafe extern "C" fn(), arg: u32) -> Option<[u32; 6]> {
    l.call(smp::OP_URUN, [f as *const () as usize as u32, arg, 0, 0, 0, 0], 100).map(|r| r.a)
}

/// A CHECK id with a prefix, built on the stack (ids are at most 48 bytes, kcore check).
struct Id {
    buf: [u8; 48],
    len: usize,
}
impl Id {
    fn new(prefix: &str, id: &str) -> Self {
        let mut buf = [0u8; 48];
        let mut len = 0;
        for b in prefix.bytes().chain(id.bytes()).take(48) {
            buf[len] = b;
            len += 1;
        }
        Self { buf, len }
    }
    fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.len]).unwrap_or("core1.bad-id")
    }
}

/// Review H1 on core 1: pmp-612's access-shape table, each stub run on core 1 under its own U view. Each must trap
/// with an accepted code and never return a value.
fn shapes(t: &mut Tally, l: &mut Link) {
    for s in &crate::bringup_shapes::SHAPES {
        let id = Id::new("core1.", s.id);
        let id = id.as_str();
        kprintln!("BEGIN {id} {:#010x}", s.addr);
        platform::flush();
        let Some([code, tval, epc, value, exited, _]) = u_run(l, s.f, s.addr as u32) else {
            record(t, id, Some(false), format_args!("no reply within 100 ms (core 1 hung?)"));
            return;
        };
        let ok = code < 32 && s.want & (1 << code) != 0 && exited == 0;
        record(
            t,
            id,
            Some(ok),
            format_args!(
                "U access at {:#010x} on core 1: code {code} ({}) mtval {tval:#x} mepc {epc:#x}, exited {exited} \
                 (want code mask {:#x})",
                s.addr,
                lichen_kcore::trap::exception_name(code as u16),
                s.want
            ),
        );
        if exited != 0 {
            kprintln!("FINDING {id}: the access RETURNED {value:#x} instead of trapping");
        }
    }
}

/// Review M1: every word of core 1's CLIC page (0x2081_0000-0x2081_1FFF, core 0's view of the other core's CLIC),
/// loaded and stored from U on core 0 with core 1 out of reset and parked. Run with pmp-612's sweep under core 0's
/// U view.
fn clic1_sweep(t: &mut Tally) {
    extern "C" {
        static _utext: u8;
    }
    // SAFETY: `_utext` is the linker's start of the U stub page (link-esp32p4.ld); naming an extern static is the only
    // unsafe part, its byte is never read, only the address is taken (review L-h).
    let utext = unsafe { &_utext as *const u8 as usize };
    let udata = platform::slot(0).data;
    crate::pmp::apply(utext, udata, UPAGE);
    crate::bringup_u::sweep(t, udata + UPAGE, "u.sweep.clic-core1", 0x2081_0000, 0x2000);
}

/// CPU clock from mcycle against mtime over 2 ms, at the mtime rate measured against SYSTIMER at boot (#654), so the
/// result is anchored to the fixed 16 MHz reference. Clears mcountinhibit.CY first if a boot stage left mcycle stopped.
/// Returns (hz, mcountinhibit as found).
fn cpu_hz() -> (u64, u32) {
    let inhibit = smp::mcountinhibit();
    if inhibit & 1 != 0 {
        smp::uninhibit_mcycle();
    }
    let (c0, m0) = (smp::cycles(), hw::clint::mtime());
    while hw::clint::mtime() < m0 + us_to_ticks(2000) {}
    let (c1, m1) = (smp::cycles(), hw::clint::mtime());
    (c1.wrapping_sub(c0) as u64 * timebase::hz() / (m1 - m0).max(1), inhibit)
}

/// One ping, timed in mcycle from just before the push to just after the pop. The deadline is checked only every
/// 1024 polls, so the CLINT read stays out of nearly every sample.
fn ping_once(l: &mut Link, seq: u32, a: [u32; 6]) -> (Option<Msg>, u32) {
    let deadline = after_ms(10);
    let c0 = smp::cycles();
    if !l.send(Msg { op: smp::OP_PING, seq, a }, deadline) {
        return (None, 0);
    }
    let mut polls = 0u32;
    loop {
        if let Some(r) = l.try_recv() {
            if r.seq != seq {
                // A late answer to an earlier timed-out request (review L1): not this ping's.
                l.drop_stale();
                continue;
            }
            return (Some(r), smp::cycles().wrapping_sub(c0));
        }
        polls = polls.wrapping_add(1);
        if polls.is_multiple_of(1024) && hw::clint::mtime() >= deadline {
            return (None, 0);
        }
    }
}

fn pings(t: &mut Tally, l: &mut Link) {
    begin("mailbox.ping");
    let (hz, inhibit) = cpu_hz();
    // SAFETY: only this function touches RTT, on core 0, and no other borrow of it exists.
    let rtt = unsafe { RTT.get() };
    let (mut ok, mut bad) = (0u32, (u32::MAX, 0u32, 0u32));
    for slot in rtt.iter_mut() {
        let seq = l.next_seq();
        let a = core::array::from_fn(|k| seq.wrapping_mul(0x9E37_79B9).rotate_left(k as u32 * 5));
        let (r, cycles) = ping_once(l, seq, a);
        *slot = cycles;
        match r {
            Some(r) if r.op == smp::OP_PING | REPLY && r.seq == seq && r.a == a.map(|x| !x) => ok += 1,
            other => {
                bad = (seq, other.map_or(0, |r| r.op), other.map_or(0, |r| r.seq));
                break;
            }
        }
    }
    record(
        t,
        "mailbox.ping",
        Some(ok as usize == PINGS),
        format_args!(
            "{ok} of {PINGS} pings answered in order with the payload complemented; first bad (seq, reply op, reply \
             seq) {bad:x?}"
        ),
    );
    let n = ok as usize;
    let lo = rtt[..n].iter().copied().min().unwrap_or(0);
    let hi = rtt[..n].iter().copied().max().unwrap_or(0);
    let (med, p95) = perf::median_p95(&mut rtt[..n]).unwrap_or((0, 0));
    let us = |c: u32| Us100(perf::cycles_to_us100(c, hz));
    // The PERF grammar (#643, v3/perf/README.md) is key=value numbers only; the conditions go on their own line.
    kprintln!("perf: kernel.mailbox_rtt = core 0 -> core 1 -> core 0 message round trip, mcycle on core 0, push to pop; both ends polled, no wake latency, no syscall work");
    kprintln!(
        "PERF kernel.mailbox_rtt median_us={} p95_us={} min_us={} max_us={} n={n} median_cycles={med} cpu_hz={hz} \
         mcountinhibit={inhibit}",
        us(med),
        us(p95),
        us(lo),
        us(hi)
    );
}

/// 16 requests back to back, reading replies as they come: the queues fill and drain while both harts run. At most
/// 16 (= the queue length) are outstanding, so core 1 never waits on a full reply queue.
fn burst(t: &mut Tally, l: &mut Link) {
    begin("mailbox.burst");
    let deadline = after_ms(100);
    let base = l.reserve(BURST);
    let mut sent = 0;
    let mut got = 0u32;
    let mut in_order = true;
    while got < BURST && hw::clint::mtime() < deadline {
        if sent < BURST && l.send(Msg { op: smp::OP_PING, seq: base + sent, a: [sent; 6] }, 0) {
            sent += 1;
        }
        if let Some(r) = l.try_recv() {
            in_order &= r.seq == base + got && r.a == [!got; 6];
            got += 1;
        }
    }
    record(
        t,
        "mailbox.burst",
        Some(got == BURST && in_order),
        format_args!("{sent} sent back to back, {got} replies, in order: {in_order}"),
    );
}

/// Core 1 stores a word through the cached alias; core 0 reads it cached (must match: one shared L1 DCache) and
/// through the uncached alias (INFO: a mismatch means the line is still dirty in that L1, which is the expected case).
fn coherence(t: &mut Tally, l: &mut Link) {
    begin("mailbox.coherence");
    let pattern = 0xC0DE_0612;
    let r = l.call(smp::OP_COHERE, [pattern, 0, 0, 0, 0, 0], 10);
    let cached = smp::COHERE_WORD.load(core::sync::atomic::Ordering::Relaxed);
    // SAFETY: COHERE_WORD is an aligned static in L2MEM.
    let raw = unsafe { hw::mailbox::read_uncached(&smp::COHERE_WORD as *const _ as usize) };
    record(
        t,
        "mailbox.coherence",
        Some(r.is_some() && cached == pattern),
        format_args!(
            "core 1 stored {pattern:#x}: core 0 cached {cached:#x}, uncached alias {raw:#x} ({})",
            if raw == pattern { "already written back" } else { "still only in the shared L1" }
        ),
    );
}

fn timer(t: &mut Tally, l: &mut Link) {
    begin("core1.timer");
    let ms = C1_TICK_US * C1_TICKS as u64 / 1000 + 1000;
    let r = l.call(smp::OP_TICK, [C1_TICK_US as u32, C1_TICKS, 0, 0, 0, 0], ms);
    let Some(r) = r else {
        record(t, "core1.timer", Some(false), format_args!("no reply within {ms} ms"));
        return;
    };
    let [count, gmin, gmax, lat_max, missed, stray] = r.a;
    let period = us_to_ticks(C1_TICK_US);
    let jitter = perf::jitter_max(period, gmin as u64, gmax as u64);
    record(
        t,
        "core1.timer",
        Some(count == C1_TICKS && missed == 0 && stray == 0 && jitter < us_to_ticks(JITTER_TARGET_US)),
        format_args!(
            "{count} of {C1_TICKS} ticks on core 1's own CLINT+CLIC, gap min/max {gmin}/{gmax} (want {period}), \
             latency max {lat_max}, missed {missed}, stray {stray}"
        ),
    );
    kprintln!(
        "perf: kernel.timer_jitter = CORE 1's tick, max |gap - {C1_TICK_US} us|; core 1 idle in wfi, core 0 polling the \
         mailbox, nothing printing"
    );
    kprintln!(
        "PERF kernel.timer_jitter max_us={} p2p_us={} n={count} period_us={C1_TICK_US}",
        Us { ticks: jitter, hz: timebase::hz() },
        Us { ticks: (gmax.saturating_sub(gmin)) as u64, hz: timebase::hz() }
    );
}

fn u_preempt(t: &mut Tally, l: &mut Link) {
    begin("core1.u-preempt");
    let r = l.call(smp::OP_USPIN, [U_SPIN, 0, 0, 0, 0, 0], 5000);
    let Some(r) = r else {
        record(t, "core1.u-preempt", Some(false), format_args!("no reply within 5 s"));
        return;
    };
    let [preempts, code, epc, ticks, _, exited] = r.a;
    let want = u_illegal_at as *const () as usize as u32;
    record(
        t,
        "core1.u-preempt",
        Some(code == EXC_ILLEGAL as u32 && epc == want && preempts >= 3 && exited == 0),
        format_args!(
            "U spin {U_SPIN} on core 1: {preempts} preemption(s) by core 1's tick, {ticks} tick(s), fault code {code} \
             ({}) at {epc:#x} (want {want:#x})",
            lichen_kcore::trap::exception_name(code as u16)
        ),
    );
}

/// #646 on core 1: every U view carries the fixed R-only CPU-subsystem window entry, so U loads there fault in the
/// hardware privilege check (code 5) and stores in the PMP (code 7) instead of hanging the hart. Core 1's own CLIC
/// (0x2080_0000), the other core's CLIC (0x2081_0000; core 0 is running, so its window is safe to reach) and the CLINT.
fn u_deny(t: &mut Tally, l: &mut Link) {
    extern "C" {
        fn u_load();
        fn u_store();
    }
    let cases: [(&str, u32, bool); 5] = [
        ("core1.u-deny.clint", 0x2000_4000, false),
        ("core1.u-deny.clic", 0x2080_0000, false),
        ("core1.u-deny.clic-core0", 0x2081_0000, false),
        ("core1.u-deny.cpusub", 0x2FFF_FFF0, false),
        ("core1.u-deny.clint-store", 0x2000_4000, true),
    ];
    for (id, addr, store) in cases {
        begin(id);
        let f: unsafe extern "C" fn() = if store { u_store } else { u_load };
        let Some([code, tval, epc, _, exited, _]) = u_run(l, f, addr) else {
            record(t, id, Some(false), format_args!("no reply within 100 ms (core 1 hung?)"));
            return;
        };
        let want = if store { 7 } else { 5 };
        record(
            t,
            id,
            Some(code == want && tval == addr && exited == 0),
            format_args!(
                "U {} {addr:#010x} on core 1: code {code} ({}) mtval {tval:#x} mepc {epc:#x} (want code {want})",
                if store { "store" } else { "load" },
                lichen_kcore::trap::exception_name(code as u16)
            ),
        );
    }
}

/// Review M-a: the CLIC driver takes ids 0..47 (16 internal + 32 external at n + 16). On core 0, line 47 (external
/// 31, unused here) is configured with ie off, read back, and put back as it was; id 48 is refused. Masked
/// throughout, so it cannot fire.
fn clic_top_line(t: &mut Tally) {
    begin("clic.line47");
    let top = hw::clic::LINES - 1;
    let before = hw::clic::line(top);
    hw::clic::configure(top, hw::clic::ATTR_M_LEVEL, 0xA0);
    let set = hw::clic::line(top);
    hw::clic::configure(top, before[2], before[3]);
    hw::clic::set_enabled(top, before[1] != 0);
    let back = hw::clic::line(top);
    let past = hw::clic::line(hw::clic::LINES);
    record(
        t,
        "clic.line47",
        Some(top == 47 && set[1] == 0 && set[2] == hw::clic::ATTR_M_LEVEL && set[3] & 0xE0 == 0xA0 && back == before),
        format_args!(
            "id {top}: ip/ie/attr/ctl before {before:?}, configured {set:?} (want ie 0, attr 0xc0, ctl top bits 0xa0), \
             restored {back:?}; id 48 reads {past:?} (refused)"
        ),
    );
}
