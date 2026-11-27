//! Review rows for the U-mode path (#612, review of d684e561): no kernel register leaks into U, more deny rows
//! (own text store, jumps into non-executable and kernel memory, core-local MMIO loads and stores), unknown ecalls,
//! the slot table, boot state (mstatus.FS/TW, HWLP and PIE state). A deny row that is NOT denied is a finding for
//! main: the rows are never loosened.
use crate::platform::{self, hw};
use crate::{kcheck, kprintln, mtrap};
use lichen_kcore::check::{Tally, Verdict};
use lichen_kcore::trap::{decode, exception_name, ENOSYS_RET};

core::arch::global_asm!(
    r#"
    .section .utext, "ax"
    .balign 4
# a0 = OR of every register the kernel could have left in U (all but sp, a0, a1 and a7 after it is set).
    .global bu_regs
bu_regs:
    or a0, ra, gp
    or a0, a0, tp
    or a0, a0, t0
    or a0, a0, t1
    or a0, a0, t2
    or a0, a0, t3
    or a0, a0, t4
    or a0, a0, t5
    or a0, a0, t6
    or a0, a0, a2
    or a0, a0, a3
    or a0, a0, a4
    or a0, a0, a5
    or a0, a0, a6
    or a0, a0, a7
    or a0, a0, s0
    or a0, a0, s1
    or a0, a0, s2
    or a0, a0, s3
    or a0, a0, s4
    or a0, a0, s5
    or a0, a0, s6
    or a0, a0, s7
    or a0, a0, s8
    or a0, a0, s9
    or a0, a0, s10
    or a0, a0, s11
    li a7, 1
    ecall
    .global bu_jump
bu_jump:
    jr a0
# An ecall with an unknown number, then exit with whatever a0 the kernel returned.
    .global bu_badcall
bu_badcall:
    li a7, 99
    ecall
    li a7, 1
    ecall
"#
);

extern "C" {
    static _utext: u8;
    static _udata: u8;
    fn u_load();
    fn u_store();
    fn bu_regs();
    fn bu_jump();
    fn bu_badcall();
}

const UPAGE: usize = 4096;

fn begin(id: &str) {
    kprintln!("BEGIN {id}");
    platform::flush();
}
fn record(t: &mut Tally, id: &str, ok: bool, detail: core::fmt::Arguments) {
    let v = if ok { Verdict::Pass } else { Verdict::Fail };
    t.record(v);
    kcheck!(id, v, "{detail}");
}
fn fnaddr(f: unsafe extern "C" fn()) -> usize {
    f as *const () as usize
}

/// Run `f` in U and say how it ended: (cause code, mtval, mepc) of the fault, or None when it exited.
fn run(f: unsafe extern "C" fn(), sp: usize, a0: usize, a1: usize) -> (Option<(u16, usize, usize)>, Option<usize>) {
    let u = mtrap::run_u(fnaddr(f), sp, a0, a1);
    (u.fault.map(|c| (decode(c.cause).code, c.tval as usize, c.epc as usize)), u.exit)
}

/// The PMP view U runs under: R-only over the CPU-subsystem window (#646), nothing at TCM; print what the table says.
fn view(t: &mut Tally) {
    begin("pmp.view");
    let e = crate::pmp::read_entries();
    let probes = [0x2080_0000u32, 0x2080_101C, 0x2000_4000, 0x3010_0000];
    let perms: [u8; 4] = core::array::from_fn(|i| lichen_kcore::pmp::u_perms(&e, probes[i]));
    record(
        t,
        "pmp.view",
        perms == [1, 1, 1, 0],
        format_args!(
            "table says U perms at CLIC {:#x}, CLIC line 7 {:#x}, CLINT {:#x}, TCM {:#x} (want 1 1 1 0: the fixed R-only window entry, hardware refuses the loads; TCM denied)",
            perms[0], perms[1], perms[2], perms[3]
        ),
    );
    for (i, x) in e.iter().enumerate().filter(|(_, x)| x.cfg != 0 || x.addr != 0) {
        kprintln!("pmp.view: entry {i:2} cfg={:#04x} addr={:#010x}", x.cfg, x.addr);
    }
}

pub fn run_rows(t: &mut Tally) {
    // SAFETY: linker symbols; only their addresses are used.
    let (utext, udata) = unsafe { (&_utext as *const u8 as usize, &_udata as *const u8 as usize) };
    let sp = udata + UPAGE;
    boot_state(t, udata);
    view(t);
    regs(t, sp);
    deny(t, utext, udata, sp);
    ecall(t, sp);
}

fn boot_state(t: &mut Tally, udata: usize) {
    begin("boot.state");
    let ms = hw::trap::mstatus();
    record(t, "boot.mstatus", ms & 0x6000 == 0, format_args!("mstatus={ms:#010x}: FS {} (want 0)", (ms >> 13) & 3));
    // TW is probably hardwired 0 on this core (WARL): set it and read it back. If so U can run wfi, which only waits
    // for the next interrupt, and the tick still preempts it.
    // SAFETY: setting mstatus.TW only makes wfi in lower modes trap.
    unsafe { core::arch::asm!("csrs mstatus, {0}", in(reg) 0x20_0000usize, options(nomem, nostack)) };
    let tw = (hw::trap::mstatus() >> 21) & 1;
    kcheck!(
        "boot.tw",
        Verdict::Info,
        "mstatus.TW after set = {tw} ({})",
        if tw == 1 { "implemented" } else { "hardwired 0 (WARL): U can run wfi, the tick still preempts it" }
    );
    record(
        t,
        "slot.layout",
        udata == lichen_kcore::slots::SLOT_DATA && platform::slot(0).data == udata,
        format_args!("_udata {udata:#x}, slot 0 data {:#x}", platform::slot(0).data),
    );
    // Read straight after boot: `_start` zeroes these, nothing in C does (a regression shows up here).
    let (h, p, c) = (hw::trap::hwlp_state(), hw::trap::pie_state(), hw::trap::mcounteren());
    record(
        t,
        "boot.coproc",
        h == 0 && p == 0 && c == 0,
        format_args!("HWLP state {h:#x}, PIE state {p:#x}, mcounteren {c:#x} (want 0, 0, 0: zeroed by _start)"),
    );
}

fn regs(t: &mut Tally, sp: usize) {
    begin("u.regs");
    let (f, exit) = run(bu_regs, sp, 0, 0);
    record(
        t,
        "u.regs",
        f.is_none() && exit == Some(0),
        format_args!("OR of ra gp tp t0-t6 a2-a7 s0-s11 as seen by U: {exit:x?} (want 0), fault {f:x?}"),
    );
}

type Case = (&'static str, unsafe extern "C" fn(), usize, u16);

/// Run each case in U and check it ends in the wanted fault at the wanted address.
fn run_cases(t: &mut Tally, sp: usize, cases: &[Case]) {
    for &(id, f, a, want) in cases {
        begin(id);
        // A store writes 0; a jump or load passes the address in a0.
        let (fault, exit) = run(f, sp, a, 0);
        let ok = fault.is_some_and(|(code, tval, _)| code == want && tval == a) && exit.is_none();
        let what = fault.unwrap_or((0xFFF, 0, 0));
        record(
            t,
            id,
            ok,
            format_args!(
                "{a:#010x}: code {} ({}) mtval {:#x} mepc {:#x}, exit {exit:x?} (want code {want})",
                what.0,
                exception_name(what.0),
                what.1,
                what.2
            ),
        );
        if !ok && fault.is_none() {
            kprintln!("FINDING {id}: U access at {a:#010x} was NOT denied");
        }
    }
}

/// Core-local accesses (#646): loads fault in the hardware privilege check, stores and fetches in the PMP.
const CORE_LOCAL: [Case; 9] = [
    ("u.deny.clint-load", u_load, 0x2000_4000, 5),
    ("u.deny.clint-store", u_store, 0x2000_4000, 7),
    ("u.deny.clic-ie-load", u_load, 0x2080_1000 + 7 * 4, 5),
    ("u.deny.clic-ie-store", u_store, 0x2080_1000 + 7 * 4, 7),
    ("u.deny.clic-load", u_load, 0x2080_0000, 5),
    ("u.deny.clic-store", u_store, 0x2080_0000, 7),
    ("u.deny.jump-clic", bu_jump, 0x2080_0000, 1),
    ("u.deny.jump-clint", bu_jump, 0x2000_4000, 1),
    ("u.deny.jump-cpusub", bu_jump, 0x2FFF_FFF0, 1),
];

fn deny(t: &mut Tally, utext: usize, udata: usize, sp: usize) {
    let cases: [Case; 10] = [
        ("u.deny.text-store", u_store, utext + 0x800, 7),
        ("u.deny.jump-data", bu_jump, udata, 1),
        ("u.deny.jump-ktext", bu_jump, 0x4FF1_0000, 1),
        ("u.deny.load-ktext", u_load, 0x4FF1_0000, 5),
        ("u.deny.jump-peri", bu_jump, 0x500C_A000, 1),
        ("u.deny.jump-uncached", bu_jump, udata + 0x4000_0000, 1),
        ("u.deny.jump-lpram", bu_jump, 0x5010_8000, 1),
        ("u.deny.jump-zero", bu_jump, 0, 1),
        ("u.deny.tcm-load", u_load, 0x3010_0000, 5),
        ("u.deny.tcm-store", u_store, 0x3010_0000, 7),
    ];
    run_cases(t, sp, &cases);
}

/// Core-local rows: LAST in the bench order, because a hang here must not hide the rows before them. Every U view
/// carries the fixed R-only window entry (kcore pmp, #646): loads reach the hardware privilege check and fault, stores
/// and fetches are denied by the PMP.
pub fn core_local_rows(t: &mut Tally) {
    // SAFETY: linker symbols; only their addresses are used.
    let (utext, udata) = unsafe { (&_utext as *const u8 as usize, &_udata as *const u8 as usize) };
    let sp = udata + UPAGE;
    // The #646 hang reproducer (probe builds only): the view WITHOUT the window entry, then one PMP-denied U load at
    // the CLINT, which hangs the P4 with no trap.
    #[cfg(feature = "probe-646")]
    {
        let plan = lichen_kcore::pmp::plan(&view_base(utext, udata), crate::pmp::gran())
            .unwrap_or_else(|_| platform::exit(4))
            .without_window();
        crate::pmp::load_unchecked(&plan);
        kprintln!("DIAG PMP-denied U load at the CLINT without the window entry: expected to hang");
        run_cases(t, sp, &CORE_LOCAL[..1]);
        return;
    }
    #[cfg(not(feature = "probe-646"))]
    {
        crate::pmp::apply(utext, udata, UPAGE);
        run_cases(t, sp, &CORE_LOCAL);
        window_probe(t, sp);
        sweep(t, sp, "u.sweep.clint", CLINT_BASE, CLINT_LEN);
        sweep(t, sp, "u.sweep.clic", CLIC_BASE, CLIC_LEN);
        sweep(t, sp, "u.sweep.userclic", USER_CLIC_BASE, CLIC_LEN);
        stride_loads(t, sp);
        // The access-shape rows go last of all: they are the likeliest to find a new hang.
        crate::bringup_shapes::run(t, sp);
        #[cfg(feature = "soak-646")]
        soak(t, sp);
    }
}

const USER_CLIC_BASE: usize = 0x20B0_0000;

/// One U load per 1 KB over the whole window outside the CLINT, CLIC and core 1's CLIC pages (swept or probed
/// elsewhere): every one must fault with code 5, none may return data. A BEGIN line per MB names a hang.
fn stride_loads(t: &mut Tally, sp: usize) {
    let (mut n, mut bad) = (0u32, 0u32);
    for mb in 0..256usize {
        let base = 0x2000_0000 + mb * 0x10_0000;
        kprintln!("BEGIN u.stride {base:#010x}");
        platform::flush();
        for k in 0..1024usize {
            let a = base + k * 0x400 + 0x40;
            if a < 0x2001_0000 || (CLIC_BASE..CLIC_BASE + 0x2_0000).contains(&a) {
                continue;
            }
            let (fault, exit) = run(u_load, sp, a, 0);
            n += 1;
            if !fault.is_some_and(|(code, tval, _)| code == 5 && tval == a) || exit.is_some() {
                bad += 1;
                if bad <= 16 {
                    kprintln!("FINDING u.stride: U load at {a:#010x} did not fault: {fault:x?} exit {exit:x?}");
                }
            }
        }
    }
    record(
        t,
        "u.stride",
        bad == 0,
        format_args!("{n} U loads, one per 1 KB over 0x20000000..0x30000000, {bad} did not fault code 5"),
    );
}

/// One U load then one U store per address, each with its own BEGIN line so a hang names the address and the access.
/// Core 1's CLIC goes last: M access to it while core 1 is in reset is a known hang (#604).
fn window_probe(t: &mut Tally, sp: usize) {
    let addrs = [0x2000_4000usize, 0x2040_0000, 0x20B0_0000, 0x2100_0000, 0x2800_0000, 0x2FFF_FFF0, 0x2081_0000];
    for a in addrs {
        for (f, want, what) in [(u_load as unsafe extern "C" fn(), 5u16, "load"), (u_store, 7, "store")] {
            kprintln!("BEGIN u.win.{what} {a:#010x}");
            platform::flush();
            let (fault, exit) = run(f, sp, a, 0);
            let ok = fault.is_some_and(|(code, tval, _)| code == want && tval == a) && exit.is_none();
            record(
                t,
                "u.win",
                ok,
                format_args!("U {what} {a:#010x}: fault {fault:x?} exit {exit:x?} (want code {want})"),
            );
        }
    }
}

/// Opt-in (feature soak-646, never in the default run): a U load at EVERY word of the window, 64M of them. Each costs
/// one U round trip with a fault; the first 4096 are timed and the estimate for the whole soak is printed first, so
/// the run can be sized (and cut with the bench timeout) before it commits.
#[cfg(feature = "soak-646")]
fn soak(t: &mut Tally, sp: usize) {
    let (mut n, mut bad) = (0u64, 0u64);
    let t0 = hw::clint::mtime();
    for a in (0x2000_0000usize..0x3000_0000).step_by(4) {
        if n % 0x40_0000 == 0 {
            kprintln!("BEGIN u.soak {a:#010x}");
            platform::flush();
        }
        let (fault, exit) = run(u_load, sp, a, 0);
        n += 1;
        if !fault.is_some_and(|(code, tval, _)| code == 5 && tval == a) || exit.is_some() {
            bad += 1;
            if bad <= 16 {
                kprintln!("FINDING u.soak: U load at {a:#010x} did not fault: {fault:x?} exit {exit:x?}");
            }
        }
        if n == 4096 {
            let us = (hw::clint::mtime() - t0) / 40;
            kprintln!("perf: u.soak: 4096 loads in {us} us, estimate for 64M loads: {} s", us * 16384 / 1_000_000);
            platform::flush();
        }
    }
    record(t, "u.soak", bad == 0, format_args!("{n} U loads over the whole window, {bad} did not fault code 5"));
}

#[cfg(feature = "probe-646")]
fn view_base(utext: usize, udata: usize) -> [lichen_kcore::pmp::Region; 2] {
    use lichen_kcore::pmp::{Region, R, W, X};
    [
        Region { base: utext as u32, end: (utext + UPAGE) as u64, perm: R | X },
        Region { base: udata as u32, end: (udata + UPAGE) as u64, perm: R | W },
    ]
}

const CLINT_BASE: usize = 0x2000_0000;
const CLINT_LEN: usize = 0x1_0000;
const CLIC_BASE: usize = 0x2080_0000;
const CLIC_LEN: usize = 0x2000;

/// U loads and stores every word of the CLINT and CLIC pages: every one must fault (loads: the hardware privilege
/// check, stores: the PMP). A word that
/// does not is a FINDING and fails the row. A BEGIN line per 4 KB block names a hang.
fn sweep(t: &mut Tally, sp: usize, id: &str, base: usize, len: usize) {
    let (mut words, mut bad) = (0u32, 0u32);
    for blk in (0..len).step_by(0x1000) {
        kprintln!("BEGIN {id}.{:#x}", base + blk);
        platform::flush();
        for a in (base + blk..base + blk + 0x1000).step_by(4) {
            for (f, want) in [(u_load as unsafe extern "C" fn(), 5u16), (u_store, 7)] {
                let (fault, exit) = run(f, sp, a, 0);
                words += 1;
                if !fault.is_some_and(|(code, tval, _)| code == want && tval == a) || exit.is_some() {
                    bad += 1;
                    if bad <= 16 {
                        kprintln!(
                            "FINDING {id}: U {} at {a:#010x} did not fault as expected: {fault:x?} exit {exit:x?}",
                            if want == 5 { "load" } else { "store" }
                        );
                    }
                }
            }
        }
    }
    record(t, id, bad == 0, format_args!("{words} U accesses over {base:#x}..{:#x}, {bad} did not fault", base + len));
}

fn ecall(t: &mut Tally, sp: usize) {
    begin("u.badcall");
    let (f, exit) = run(bu_badcall, sp, 7, 0);
    record(
        t,
        "u.badcall",
        f.is_none() && exit == Some(ENOSYS_RET),
        format_args!("unknown ecall returns {exit:x?} (want {ENOSYS_RET:#x}) and the run goes on, fault {f:x?}"),
    );
}
