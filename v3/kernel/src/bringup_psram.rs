//! #621 bench rows on core 0: PSRAM up, two apps' stacks and heaps in PSRAM, each behind its own PMP view, and
//! freed memory reads zero. The apps are the U stubs (`u_load`, `u_store` on the stub page) run with `run_u`; each
//! view grants the stub page and the app's own PSRAM block, which is also its stack.
//!
//! Every access to another app's block and to either app's uncached alias must fault with mtval = the address. A
//! PMP-denied U access to 0x48xx or 0x88xx is new on this silicon (#646 hung on another window): each row is
//! announced with `begin`, so a hang names it.
use crate::bringup::{begin, record};
use crate::platform::{self, hw};
use crate::{mtrap, psram};
use lichen_kcore::check::Tally;
use lichen_kcore::palloc::{Block, Error, Kind, PAGE, UNCACHED_BASE};
use lichen_kcore::pmp::{Region, R, X};

extern "C" {
    static _utext: u8;
    fn u_load();
    fn u_store();
}

/// Stack and heap of each app.
const BLOCK: usize = 0x1_0000;
const QUOTA: usize = 2 * BLOCK;
const UPAGE: usize = 4096;
const LOAD_FAULT: u16 = 5;
const STORE_FAULT: u16 = 7;
const FETCH_FAULT: u16 = 1;
const MARK: usize = 0x6210_A5A5;

fn fnaddr(f: unsafe extern "C" fn()) -> usize {
    f as *const () as usize
}
fn off(r: &Result<(lichen_kcore::palloc::Handle, Block), Error>) -> usize {
    r.map_or(usize::MAX, |x| x.1.off)
}
fn cached(b: &Block) -> usize {
    psram::grant(b).map_or(0, |r| r.base as usize)
}
fn uncached(b: &Block) -> usize {
    UNCACHED_BASE + b.off
}

/// Load the view of an app that owns `b`: the stub page R+X and the block R+W. Returns its sp (the block's top).
fn view_of(b: &Block) -> usize {
    // SAFETY: a linker symbol; only its address is used.
    let utext = unsafe { &_utext as *const u8 as usize };
    let Some(r) = psram::grant(b) else { platform::exit(4) };
    crate::pmp::load(
        crate::pmp::checked(&[Region { base: utext as u32, end: (utext + UPAGE) as u64, perm: R | X }, r]).plan(),
    );
    r.end as usize
}

pub fn run(t: &mut Tally) {
    begin("psram.init");
    let info = match psram::up() {
        Ok(i) => i,
        Err(why) => return record(t, "psram.init", Some(false), format_args!("{why} failed: no PSRAM rows")),
    };
    let tune = info.tuned.map(|t| (t.phase_id as u32, t.data_dl, t.dqs_dl)).unwrap_or_default();
    record(
        t,
        "psram.init",
        Some(info.size_bytes as usize >= hw::psram::map::MAPPED_BYTES),
        format_args!(
            "{} MB at {} MHz, CPU on the ROM clock; ldo {} {}, mpll {}, mr0 {:#x} mr4 {:#x} mr8 {:#x}, tune {} {} {}",
            info.size_bytes >> 20,
            hw::psram::current_mhz(),
            info.ldo_dref,
            info.ldo_mul,
            info.mpll_mhz,
            info.modes.mr0,
            info.modes.mr4,
            info.modes.mr8,
            tune.0,
            tune.1,
            tune.2
        ),
    );
    let Some((a, b)) = alloc_rows(t) else { return };
    let a_sp = view_of(&a);
    own_rows(t, &a, a_sp);
    deny_rows(t, &a, &b, a_sp);
    zero_row(t, a, b);
}

/// Owners 0 and 1 get a data block each; an exec block for 0; a third block over the quota is refused.
fn alloc_rows(t: &mut Tally) -> Option<(Block, Block)> {
    begin("psram.alloc");
    let q = psram::set_quota(0, QUOTA).and(psram::set_quota(1, QUOTA));
    let (a, b, e) =
        (psram::alloc(0, Kind::Data, BLOCK), psram::alloc(1, Kind::Data, BLOCK), psram::alloc(0, Kind::Exec, PAGE));
    let over = psram::alloc(0, Kind::Data, BLOCK);
    let ok = q.is_ok() && a.is_ok() && b.is_ok() && e.is_ok() && over == Err(Error::Quota);
    record(
        t,
        "psram.alloc",
        Some(ok),
        format_args!(
            "offsets A {:#x} B {:#x} A exec {:#x}, A over quota refused {}, usage A {} B {}",
            off(&a),
            off(&b),
            off(&e),
            over == Err(Error::Quota),
            psram::usage(0),
            psram::usage(1)
        ),
    );
    Some((a.ok()?.1, b.ok()?.1))
}

/// A stores and loads its own block through the cached alias; M sees the value there too.
fn own_rows(t: &mut Tally, a: &Block, sp: usize) {
    begin("psram.u.own");
    let at = cached(a) + 64;
    let st = mtrap::run_u(fnaddr(u_store), sp, at, MARK);
    let ld = mtrap::run_u(fnaddr(u_load), sp, at, 0);
    let m = psram::read_cached(a.off + 64);
    record(
        t,
        "psram.u.own",
        Some(st.exit.is_some() && ld.exit == Some(MARK) && m as usize == MARK),
        format_args!("A at {at:#x}: store {}, load {:#x}, M reads {m:#x}", st.exit.is_some(), ld.exit.unwrap_or(0)),
    );
}

/// Each access must fault with the right code and mtval; B's word must be unchanged afterwards.
fn deny_rows(t: &mut Tally, a: &Block, b: &Block, sp: usize) {
    let before = psram::read_cached(b.off);
    let cases: [(&str, usize, u16); 7] = [
        ("psram.deny.other-load", cached(b), LOAD_FAULT),
        ("psram.deny.other-store", cached(b), STORE_FAULT),
        ("psram.deny.other-uncached", uncached(b), LOAD_FAULT),
        ("psram.deny.own-uncached-load", uncached(a), LOAD_FAULT),
        ("psram.deny.own-uncached-store", uncached(a) + 64, STORE_FAULT),
        ("psram.deny.guard", cached(a) + BLOCK, STORE_FAULT),
        ("psram.deny.exec-data", cached(a), FETCH_FAULT),
    ];
    for (id, at, want) in cases {
        begin(id);
        let u = match want {
            LOAD_FAULT => mtrap::run_u(fnaddr(u_load), sp, at, 0),
            STORE_FAULT => mtrap::run_u(fnaddr(u_store), sp, at, 0xBAD),
            _ => mtrap::run_u(at, sp, 0, 0),
        };
        let (code, tval) = u.fault.map_or((0xFFF, 0), |f| (lichen_kcore::trap::decode(f.cause).code, f.tval as usize));
        record(
            t,
            id,
            Some(code == want && tval == at && u.exit.is_none()),
            format_args!("A at {at:#010x}: code {code} (want {want}) mtval {tval:#x}"),
        );
    }
    let after = psram::read_cached(b.off);
    record(
        t,
        "psram.deny.intact",
        Some(before == after),
        format_args!("B's word {before:#x} before, {after:#x} after"),
    );
}

/// A's block is filled and written back (the chip must hold the fill: proof that write-back at the ROM clock reaches
/// PSRAM, and that the zeros after the release are not the alloc-time ones), released, and must read zero through both
/// aliases; owner 2 gets the same pages back and its U load reads 0.
fn zero_row(t: &mut Tally, a: Block, b: Block) {
    begin("psram.zero");
    let words = (a.off..a.off + a.len).step_by(4);
    for o in words.clone() {
        // SAFETY: A's block, mapped, owned by the kernel while no U runs.
        unsafe { core::ptr::write_volatile((cached(&a) + o - a.off) as *mut u32, (o ^ MARK) as u32) };
    }
    let wb = psram::writeback(a.off, a.len);
    let bad_fill = words.clone().filter(|&o| psram::read_uncached(o) != (o ^ MARK) as u32).count();
    // A's process ends: its blocks come back through the same path an exit takes.
    let zeroed = psram::release_owner(0);
    let bad_c = words.clone().filter(|&o| psram::read_cached(o) != 0).count();
    let bad_u = words.filter(|&o| psram::read_uncached(o) != 0).count();
    let _ = psram::set_quota(2, QUOTA);
    let c = psram::alloc(2, Kind::Data, BLOCK);
    let c_off = off(&c);
    // usize::MAX: no reuse to test (another block came back, or none).
    let reuse = match c {
        Ok((_, c)) if c.off == a.off => mtrap::run_u(fnaddr(u_load), view_of(&c), cached(&c) + 64, 0).exit,
        _ => None,
    }
    .unwrap_or(usize::MAX);
    let released = psram::release_owner(1) && psram::release_owner(2) && psram::usage(0) + psram::usage(1) == 0;
    record(
        t,
        "psram.zero",
        Some(wb && bad_fill == 0 && zeroed && bad_c == 0 && bad_u == 0 && reuse == 0 && released),
        format_args!(
            "A's {} words filled, written back {wb}, chip differs from the fill in {bad_fill}; released: zeroed {zeroed}, non-zero cached {bad_c} uncached {bad_u}; owner 2 got \
             {c_off:#x} (A {:#x}, B {:#x}), its U load reads {reuse:#x}; all released {released}",
            a.len / 4,
            a.off,
            b.off
        ),
    );
}
