//! Checkpoint 2 bench rows (#612): PMP discovery and planner on the real chip, the PMA manager, and the PERF lines
//! (full per-app PMP reload, U ecall round trip). Runs after the first U rows, in the same bring-up image.
use crate::platform::{self, hw};
use crate::{kcheck, kprintln, mtrap, pmp};
use core::sync::atomic::{AtomicU8, Ordering};
use lichen_kcore::check::{Tally, Verdict};
use lichen_kcore::{perf, pma as ka, pmp as kp};

// U-side loop for the round-trip timing, in the U stub page (the kernel's boot S owns the other stubs): n echo ecalls
// (a7 = 0), then the exit ecall. The CSR stubs each hold exactly one CSR instruction at their entry, so the bring-up
// can arm `expect_fault` at the function address and survive a CSR this core does not implement.
core::arch::global_asm!(
    r#"
    .section .utext, "ax"
    .balign 4
# U-side timing loop: a0 = n, a1 = sample buffer. Each iteration brackets one echo ecall with rdcycle (needs
# mcounteren.CY, set by the bring-up in this measurement build only) and stores the cycle delta. t0 counts: the echo
# handler adds 1 to a0 on every call. Then the exit ecall.
    .global bp_u_echo_time
bp_u_echo_time:
    mv t0, a0
    beqz t0, 2f
1:  rdcycle t3
    li a7, 0
    ecall
    rdcycle t4
    sub t4, t4, t3
    sw t4, 0(a1)
    addi a1, a1, 4
    addi t0, t0, -1
    bnez t0, 1b
2:  li a7, 1
    ecall

    .text
    .balign 4
    .option push
    .option norvc
    .global bp_read_mcycle
bp_read_mcycle:
    csrr a0, mcycle
    ret
    .global bp_read_inhibit
bp_read_inhibit:
    csrr a0, 0x320
    ret
    .global bp_clear_inhibit
bp_clear_inhibit:
    csrw 0x320, zero
    ret
    .global bp_write_counteren
bp_write_counteren:
    csrw 0x306, a0
    ret
    .option pop
"#
);

extern "C" {
    static _utext: u8;
    static _udata: u8;
    fn u_load();
    fn u_store();
    fn u_echo();
    fn bp_u_echo_time();
    fn bp_read_mcycle() -> u32;
    fn bp_read_inhibit() -> u32;
    fn bp_clear_inhibit();
    fn bp_write_counteren(v: usize);
}

const UPAGE: usize = 4096;
const MTIME_HZ: u64 = 40_000_000;

fn begin(id: &str) {
    kprintln!("BEGIN {id}");
    platform::flush();
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

fn fnaddr(f: unsafe extern "C" fn()) -> usize {
    f as *const () as usize
}

fn addrs() -> (usize, usize) {
    // SAFETY: linker symbols; only their addresses are used.
    unsafe { (&_utext as *const u8 as usize, &_udata as *const u8 as usize) }
}

pub fn run(t: &mut Tally) {
    let (utext, udata) = addrs();
    discover(t);
    tor_rows(t, utext, udata);
    pma_rows(t, udata);
    perf_rows(t, utext, udata);
    pmp::apply(utext, udata, UPAGE);
}

/// Discovery by readback in the ROM state: 16 free entries, 128 B granularity, none locked.
fn discover(t: &mut Tally) {
    begin("pmp.discover");
    let d = pmp::discovery();
    record(
        t,
        "pmp.discover",
        Some(d.free() == 16 && d.locked_mask == 0 && d.gran == Some(kp::GRAN_P4)),
        format_args!(
            "{} of 16 entries free, locked mask {:#06x}, granularity {:?} (want 16, 0, 128)",
            d.free(),
            d.locked_mask,
            d.gran
        ),
    );
}

/// A U fault at `a` of the given kind: (cause code, mtval).
fn u_fault(f: unsafe extern "C" fn(), sp: usize, a: usize, v: usize) -> (u16, usize, bool) {
    let u = mtrap::run_u(fnaddr(f), sp, a, v);
    u.fault.map_or((0xFFF, 0, u.exit.is_some()), |c| (lichen_kcore::trap::decode(c.cause).code, c.tval as usize, false))
}

/// A plan that needs TOR entries (base holder + top) works on hardware: grant at both ends, deny one granule outside.
fn tor_rows(t: &mut Tally, utext: usize, udata: usize) {
    begin("pmp.plan");
    let regions = [
        kp::Region { base: utext as u32, end: (utext + UPAGE) as u64, perm: kp::R | kp::X },
        kp::Region { base: (udata + 128) as u32, end: (udata + UPAGE) as u64, perm: kp::R | kp::W },
    ];
    let plan = kp::plan(&regions, pmp::gran());
    let Ok(plan) = plan else {
        record(t, "pmp.plan", Some(false), format_args!("planner refused: {plan:?}"));
        return;
    };
    let writes = pmp::load(&plan);
    record(
        t,
        "pmp.plan",
        Some(plan.used() == 3 && plan.entries()[1].cfg == 0 && plan.entries()[2].cfg == (kp::TOR | kp::R | kp::W)),
        format_args!(
            "{} entries (NAPOT text, TOR base+top data), {writes} register write(s), read back verified",
            plan.used()
        ),
    );
    let sp = udata + UPAGE;
    begin("pmp.tor");
    let last = udata + UPAGE - 4;
    let st = u_fault(u_store, sp, last, 0x6120_0612);
    let ld = mtrap::run_u(fnaddr(u_load), sp, last, 0).exit;
    let first = u_fault(u_store, sp, udata + 128, 1);
    let below = u_fault(u_store, sp, udata + 124, 1);
    let above = u_fault(u_load, sp, udata + UPAGE, 0);
    record(
        t,
        "pmp.tor",
        Some(
            st.2 && ld == Some(0x6120_0612)
                && first.2
                && below == (7, udata + 124, false)
                && above == (5, udata + UPAGE, false),
        ),
        format_args!(
            "last word ok {} load {ld:x?}, first word ok {}, 4 B below base: code {} mtval {:#x}, first byte past top: \
             code {} mtval {:#x}",
            st.2, first.2, below.0, below.1, above.0, above.1
        ),
    );
}

/// The PMA manager: adds cacheable entries for L2MEM, flash and PSRAM below the ROM whitelist, unlocked, with fences.
fn pma_rows(t: &mut Tally, udata: usize) {
    begin("pma.table");
    let mut table = [ka::Entry::default(); ka::ENTRIES];
    for (i, e) in table.iter_mut().enumerate() {
        let (cfg, addr) = hw::pma::read(i);
        *e = ka::Entry { cfg, addr };
        if cfg != 0x0800_0000 || addr != 0 {
            kprintln!("pma: entry {i:2} cfg={cfg:#010x} addr={addr:#010x}");
        }
    }
    let ok = ka::programmable(&table);
    record(
        t,
        "pma.table",
        None,
        format_args!("ROM state recognised (entry 15 = whitelist, kernel slots free and unlocked): {ok}"),
    );
    if !ok {
        record(t, "pma.program", None, format_args!("skipped: not the ROM table, PMA left untouched"));
        return;
    }
    begin("pma.program");
    for (i, e) in ka::kernel_entries() {
        hw::pma::write(i, e.cfg, e.addr);
    }
    hw::pma::fence();
    let mut matches = true;
    let mut lock_bits = 0u32;
    for (i, e) in ka::kernel_entries() {
        matches &= hw::pma::read(i) == (e.cfg, e.addr);
    }
    for i in 0..hw::pma::ENTRIES {
        lock_bits += hw::pma::read(i).0 >> 29 & 1;
    }
    record(
        t,
        "pma.program",
        Some(matches && lock_bits == 0),
        format_args!(
            "entries 0..5 (L2MEM, flash, PSRAM cacheable RWX; 2 fences) read back {matches}, L bits set: {lock_bits}"
        ),
    );
    begin("pma.live");
    // The kernel still runs, M still reaches cached L2MEM, and a U round trip still works with the new table.
    // SAFETY: the U data page is NOLOAD kernel-owned memory.
    let m = unsafe {
        core::ptr::write_volatile(udata as *mut u32, 0x1357_9BDF);
        core::ptr::read_volatile(udata as *const u32)
    };
    let u = mtrap::run_u(fnaddr(u_echo), udata + UPAGE, 0x40, 0);
    record(
        t,
        "pma.live",
        Some(m == 0x1357_9BDF && u.exit == Some(0x41)),
        format_args!("M store+load of cached L2MEM {m:#x}, U echo exit {:x?}", u.exit),
    );
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Src {
    Mtime = 0,
    Mcycle = 1,
}
static SRC: AtomicU8 = AtomicU8::new(0);
static HZ: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(MTIME_HZ as u32);

fn src() -> Src {
    if SRC.load(Ordering::Relaxed) == 1 {
        Src::Mcycle
    } else {
        Src::Mtime
    }
}
fn now() -> u32 {
    match src() {
        // SAFETY: `calibrate` proved the stub works before the source was selected.
        Src::Mcycle => unsafe { bp_read_mcycle() },
        Src::Mtime => hw::clint::mtime() as u32,
    }
}
fn hz() -> u64 {
    HZ.load(Ordering::Relaxed) as u64
}

/// Run a one-CSR stub and say whether it executed (true) or faulted illegal (false).
fn survives(stub: usize, run: impl FnOnce()) -> bool {
    mtrap::expect_fault(stub as u32);
    run();
    mtrap::take_caught().is_none()
}

/// The kernel's cycle counter is mcycle (the P4 has no other): read mcountinhibit, clear it, calibrate against mtime
/// (40 MHz). Falls back to mtime ticks (25 ns) when mcycle does not count.
fn calibrate(t: &mut Tally) {
    begin("perf.clock");
    // SAFETY: stubs hold one CSR instruction each; a fault is caught by expect_fault.
    let inhibit = unsafe { bp_read_inhibit() };
    survives(bp_clear_inhibit as *const () as usize, || unsafe { bp_clear_inhibit() });
    let usable = survives(bp_read_mcycle as *const () as usize, || unsafe {
        bp_read_mcycle();
    });
    if usable {
        SRC.store(Src::Mcycle as u8, Ordering::Relaxed);
        let (c0, m0) = (now(), hw::clint::mtime());
        while hw::clint::mtime() - m0 < 4000 {} // 100 us
        let (c1, m1) = (now(), hw::clint::mtime());
        let f = perf::clock_hz(c1.wrapping_sub(c0), (m1 - m0) as u32, MTIME_HZ);
        if (10_000_000..1_000_000_000).contains(&f) {
            HZ.store(f as u32, Ordering::Relaxed);
            record(
                t,
                "perf.clock",
                None,
                format_args!(
                    "timebase mcycle, CPU clock {} MHz (calibrated against mtime 40 MHz), mcountinhibit at boot {inhibit:#x}",
                    f / 1_000_000
                ),
            );
            return;
        }
        kprintln!("perf: mcycle counted {} over {} mtime ticks -> {f} Hz", c1.wrapping_sub(c0), m1 - m0);
    }
    SRC.store(Src::Mtime as u8, Ordering::Relaxed);
    HZ.store(MTIME_HZ as u32, Ordering::Relaxed);
    record(
        t,
        "perf.clock",
        None,
        format_args!("timebase mtime 40 MHz (25 ns); mcycle not usable, mcountinhibit at boot {inhibit:#x}"),
    );
}

fn print_perf(name: &str, samples: &mut [u32], note: &str) {
    let hz = hz();
    let Some((med, p95)) = perf::median_p95(samples) else { return };
    let (min, max) = (samples[0], samples[samples.len() - 1]); // sorted by median_p95
    let us = |c: u32| {
        let ns = perf::ticks_to_ns(c, hz);
        (ns / 1000, (ns % 1000) / 10)
    };
    let (lo, m, p, hi) = (us(min), us(med), us(p95), us(max));
    let sname = if src() == Src::Mcycle { "mcycle" } else { "mtime" };
    // Conditions go on their own line: perf_report.py accepts only key=number pairs on a PERF line.
    kprintln!("perf: {name}: timebase {sname}, {note}");
    kprintln!(
        "PERF {name} median_us={}.{:02} p95_us={}.{:02} min_us={}.{:02} max_us={}.{:02} n={} clock_mhz={} cpu_hz={}",
        m.0,
        m.1,
        p.0,
        p.1,
        lo.0,
        lo.1,
        hi.0,
        hi.1,
        samples.len(),
        hz / 1_000_000,
        hz
    );
}

const N: usize = 128;

fn perf_rows(t: &mut Tally, utext: usize, udata: usize) {
    calibrate(t);
    reload_perf(utext, udata);
    syscall_perf(udata);
    record(t, "perf.done", None, format_args!("PERF lines above"));
}

/// Eight distinct NAPOT regions (never executed): every reload rewrites all eight addresses and the cfg bytes.
fn eight(base: u32, swap: bool) -> kp::Plan {
    let rs: [kp::Region; 8] = core::array::from_fn(|k| {
        let rw = (k % 2 == 0) != swap;
        kp::Region {
            base: base + k as u32 * 0x2000,
            end: (base + k as u32 * 0x2000 + 0x1000) as u64,
            perm: if rw { kp::R | kp::W } else { kp::R | kp::X },
        }
    });
    kp::plan(&rs, pmp::gran()).unwrap_or_else(|_| platform::exit(4))
}

fn two(base: u32) -> kp::Plan {
    let rs = [
        kp::Region { base, end: base as u64 + 0x1000, perm: kp::R | kp::X },
        kp::Region { base: base + 0x2000, end: base as u64 + 0x3000, perm: kp::R | kp::W },
    ];
    kp::plan(&rs, pmp::gran()).unwrap_or_else(|_| platform::exit(4))
}

/// One reload = diff against the shadow, register writes, readback verify (`pmp::load`), alternating two plans.
fn reload_perf(utext: usize, udata: usize) {
    begin("perf.pmp");
    let mut s = [0u32; N];
    let (a, b) = (eight(0x4FE0_0000, false), eight(0x4FD0_0000, true));
    for (i, v) in s.iter_mut().enumerate() {
        let p = if i % 2 == 0 { &a } else { &b };
        let t0 = now();
        pmp::load(p);
        *v = now().wrapping_sub(t0);
    }
    print_perf(
        "pmp.reload",
        &mut s,
        "8 entries (all addresses and cfg bytes differ), reload = diff + writes + readback verify",
    );
    let (a, b) = (two(0x4FE0_0000), two(0x4FD0_0000));
    for (i, v) in s.iter_mut().enumerate() {
        let p = if i % 2 == 0 { &a } else { &b };
        let t0 = now();
        pmp::load(p);
        *v = now().wrapping_sub(t0);
    }
    print_perf("pmp.reload.app2", &mut s, "2 regions (text+data), two apps alternating, with readback verify");
    let rs = [
        kp::Region { base: utext as u32, end: (utext + UPAGE) as u64, perm: kp::R | kp::X },
        kp::Region { base: udata as u32, end: (udata + UPAGE) as u64, perm: kp::R | kp::W },
    ];
    for v in s.iter_mut() {
        let t0 = now();
        let p = kp::plan(&rs, pmp::gran());
        *v = now().wrapping_sub(t0);
        if p.is_err() {
            platform::exit(4);
        }
    }
    print_perf("pmp.plan", &mut s, "planner only, 2 regions");
}

/// U ecall round trip, timed in U with rdcycle around each echo ecall (ecall -> M handler -> mret -> next
/// instruction), no tick but run_u's 10 ms watchdog. mcounteren.CY is set for this measurement only: it lets U read the cycle counter, a timing
/// side channel, so it is cleared again straight after.
fn syscall_perf(udata: usize) {
    begin("perf.syscall");
    pmp::apply(addrs().0, udata, UPAGE);
    if src() != Src::Mcycle {
        kprintln!("perf: syscall.local skipped: no cycle counter");
        return;
    }
    const SAMPLES: usize = 256;
    // SAFETY: stub with one CSR instruction; mcounteren bit 0 = CY.
    unsafe { bp_write_counteren(1) };
    let u = mtrap::run_u(fnaddr(bp_u_echo_time), udata + UPAGE, SAMPLES, udata);
    // SAFETY: as above.
    unsafe { bp_write_counteren(0) };
    if u.echoes as usize != SAMPLES || u.fault.is_some() {
        kprintln!("perf: syscall.local skipped: {} echoes, fault {}", u.echoes, u.fault.is_some());
        return;
    }
    let mut s = [0u32; SAMPLES];
    for (i, v) in s.iter_mut().enumerate() {
        // SAFETY: the U data page is kernel-owned NOLOAD memory U just filled with SAMPLES words.
        *v = unsafe { core::ptr::read_volatile((udata + 4 * i) as *const u32) };
    }
    print_perf(
        "syscall.local",
        &mut s,
        "U rdcycle around one echo ecall (two rdcycle included), only the 10 ms run_u watchdog tick",
    );
}
