//! #621 bench rows on core 0: PSRAM up, two apps' memory in PSRAM, each behind its own PMP view, and freed memory
//! reads zero. The apps are the U stubs (`u_load`, `u_store` on the stub page) run with `run_u`; each view grants the
//! stub page and the app's own PSRAM block, and sp points at the block's top (the stubs never push).
//!
//! Every access to another app's block, to either app's uncached alias and to the excluded range must fault with
//! mtval = the address. A PMP-denied U access to 0x48xx or 0x88xx is new on this silicon (#646 hung on another
//! window): each row is announced with `begin`, so a hang names it. Before any release the stub-only view is loaded:
//! `psram::release_owner` refuses while a loaded view still reaches the blocks.
use crate::bringup::{begin, record};
use crate::platform::{self, hw};
use crate::{mtrap, psram};
use lichen_kcore::check::Tally;
use lichen_kcore::palloc::{Block, Error, Handle, Kind, CACHED_BASE, HOLE_START, PAGE, UNCACHED_BASE};
use lichen_kcore::pmp::{Region, R, X};

extern "C" {
    static _utext: u8;
    fn u_load();
    fn u_store();
}

/// Memory of each app.
const BLOCK: usize = 0x1_0000;
const QUOTA: usize = 2 * BLOCK;
const UPAGE: usize = 4096;
const LOAD_FAULT: u16 = 5;
const STORE_FAULT: u16 = 7;
const FETCH_FAULT: u16 = 1;
const ILLEGAL: u16 = 2;
const NO_FAULT: u16 = 0xFFF;
const MARK: usize = 0x6210_A5A5;

/// A block as the kernel holds it: the handle is what grants and code loads go through.
#[derive(Clone, Copy)]
struct App {
    owner: usize,
    h: Handle,
    b: Block,
}

fn get(owner: usize, kind: Kind, len: usize) -> Result<App, Error> {
    psram::alloc(owner, kind, len).map(|(h, b)| App { owner, h, b })
}
fn off(r: &Result<App, Error>) -> usize {
    r.map_or(usize::MAX, |x| x.b.off)
}
fn fnaddr(f: unsafe extern "C" fn()) -> usize {
    f as *const () as usize
}
/// Address arithmetic only (targets of the deny rows); access goes through `psram::grant`.
fn cached(b: &Block) -> usize {
    CACHED_BASE + b.off
}
fn uncached(b: &Block) -> usize {
    UNCACHED_BASE + b.off
}
fn code_of(u: &mtrap::URun) -> u16 {
    u.fault.map_or(NO_FAULT, |f| lichen_kcore::trap::decode(f.cause).code)
}

fn stub_region() -> Region {
    // SAFETY: a linker symbol; only its address is used.
    let utext = unsafe { &_utext as *const u8 as usize };
    Region { base: utext as u32, end: (utext + UPAGE) as u64, perm: R | X }
}

/// Load the view of `app`: the stub page R+X and its block. Returns sp (the block's top).
fn view_of(app: &App) -> usize {
    let Some(r) = psram::grant(app.owner, app.h) else { platform::exit(4) };
    crate::pmp::load(crate::pmp::checked(&[stub_region(), r]).plan());
    r.end as usize
}

/// The stub page only: no PSRAM is reachable, so blocks may be released.
fn stub_view() {
    crate::pmp::load(crate::pmp::checked(&[stub_region()]).plan());
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
    let Some((a, ax, b)) = alloc_rows(t) else { return };
    limit_row(t);
    let a_sp = view_of(&a);
    own_rows(t, &a, &b, a_sp);
    deny_rows(t, &a.b, &b.b, a_sp);
    zero_row(t, a, ax, b);
    icache_row(t);
    let errs = psram::mspi_errors();
    record(t, "psram.mspi", Some(errs == 0), format_args!("MSPI FIFO error flags {errs:#x} after the rows (want 0)"));
}

/// Owners 0 and 1 get a data block each; an exec block for 0; a block over 0's quota is refused.
fn alloc_rows(t: &mut Tally) -> Option<(App, App, App)> {
    begin("psram.alloc");
    let q = psram::set_quota(0, QUOTA).and(psram::set_quota(1, QUOTA));
    let (a, b, e) = (get(0, Kind::Data, BLOCK), get(1, Kind::Data, BLOCK), get(0, Kind::Exec, PAGE));
    let over = get(0, Kind::Data, BLOCK).map(|x| x.b.off);
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
    Some((a.ok()?, e.ok()?, b.ok()?))
}

/// One owner holds at most OWNER_BLOCKS blocks, whatever its quota (each is a PMP entry).
fn limit_row(t: &mut Tally) {
    begin("psram.limit");
    let _ = psram::set_quota(5, 1 << 20);
    let held = (0..lichen_kcore::palloc::OWNER_BLOCKS).filter(|_| get(5, Kind::Data, PAGE).is_ok()).count();
    let next = get(5, Kind::Data, PAGE).map(|x| x.b.off);
    let released = psram::release_owner(5);
    record(
        t,
        "psram.limit",
        Some(held == lichen_kcore::palloc::OWNER_BLOCKS && next == Err(Error::BlockLimit) && released == Some(true)),
        format_args!("owner 5 got {held} page blocks, the next refused {}, released {released:?}", next.is_err()),
    );
}

/// A stores and loads its own block through the cached alias, and M sees the value. The kernel refuses A's handle
/// to anyone else, and a handle it never issued.
fn own_rows(t: &mut Tally, a: &App, b: &App, sp: usize) {
    begin("psram.u.own");
    let at = cached(&a.b) + 64;
    let st = mtrap::run_u(fnaddr(u_store), sp, at, MARK);
    let ld = mtrap::run_u(fnaddr(u_load), sp, at, 0);
    let m = psram::read_cached(a.b.off + 64);
    record(
        t,
        "psram.u.own",
        Some(st.exit.is_some() && ld.exit == Some(MARK) && m as usize == MARK),
        format_args!("A at {at:#x}: store {}, load {:#x}, M reads {m:#x}", st.exit.is_some(), ld.exit.unwrap_or(0)),
    );
    let foreign = psram::grant(b.owner, a.h).is_none() && psram::grant(a.owner, b.h).is_none();
    let forged = psram::grant(a.owner, Handle(a.h.0 ^ 0x100)).is_none();
    let code = !psram::load_code(a.owner, a.h, &EXIT1);
    record(
        t,
        "psram.grant",
        Some(foreign && forged && code),
        format_args!(
            "foreign handles refused {foreign}, a wrong generation refused {forged}, code into data refused {code}"
        ),
    );
}

/// Each access must fault with the right code and mtval; B's word must be unchanged afterwards.
fn deny_rows(t: &mut Tally, a: &Block, b: &Block, sp: usize) {
    // SAFETY: B's block, mapped, kernel-owned while A runs; a mark so a store that got through shows.
    unsafe { core::ptr::write_volatile(cached(b) as *mut u32, MARK as u32) };
    let cases: [(&str, usize, u16); 9] = [
        ("psram.deny.other-load", cached(b), LOAD_FAULT),
        ("psram.deny.other-store", cached(b), STORE_FAULT),
        ("psram.deny.other-uncached", uncached(b), LOAD_FAULT),
        ("psram.deny.other-uncached-store", uncached(b) + 64, STORE_FAULT),
        ("psram.deny.own-uncached-load", uncached(a), LOAD_FAULT),
        ("psram.deny.own-uncached-store", uncached(a) + 64, STORE_FAULT),
        // The page after A's block (its guard). A's view ends at the block, so this is the PMP's deny, not a guard
        // page in the view; the allocator keeps the guard free (host proptests).
        ("psram.deny.past-end", cached(a) + BLOCK, STORE_FAULT),
        ("psram.deny.hole", CACHED_BASE + HOLE_START, LOAD_FAULT),
        ("psram.deny.exec-data", cached(a), FETCH_FAULT),
    ];
    for (id, at, want) in cases {
        begin(id);
        let u = match want {
            LOAD_FAULT => mtrap::run_u(fnaddr(u_load), sp, at, 0),
            STORE_FAULT => mtrap::run_u(fnaddr(u_store), sp, at, 0xBAD),
            _ => mtrap::run_u(at, sp, 0, 0),
        };
        let (code, tval) = (code_of(&u), u.fault.map_or(0, |f| f.tval as usize));
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
        Some(after as usize == MARK && psram::read_cached(b.off + 64) == 0),
        format_args!("B's words {after:#x} (want {MARK:#x}) and {:#x} (want 0)", psram::read_cached(b.off + 64)),
    );
}

/// Fill `b` with offset ^ MARK and write it back; returns the words where the chip differs from the fill.
fn fill(b: &Block) -> usize {
    for o in (b.off..b.off + b.len).step_by(4) {
        // SAFETY: the block is mapped and kernel-owned while no U runs.
        unsafe { core::ptr::write_volatile((cached(b) + o - b.off) as *mut u32, (o ^ MARK) as u32) };
    }
    if !psram::writeback(b.off, b.len) {
        return usize::MAX;
    }
    (b.off..b.off + b.len).step_by(4).filter(|&o| psram::read_uncached(o) != (o ^ MARK) as u32).count()
}

/// Non-zero words of `b` through (cached, uncached).
fn nonzero(b: &Block) -> (usize, usize) {
    let w = (b.off..b.off + b.len).step_by(4);
    (w.clone().filter(|&o| psram::read_cached(o) != 0).count(), w.filter(|&o| psram::read_uncached(o) != 0).count())
}

/// A's data and exec blocks are filled and written back (the chip must hold the fill: write-back at the ROM clock
/// reaches PSRAM, and the zeros afterwards are not the alloc-time ones). Releasing A while its view is loaded is
/// refused; with the stub view it goes through, and both blocks read zero through both aliases. A's handle is dead
/// after that; owner 2 gets A's data pages back and its U load reads 0.
fn zero_row(t: &mut Tally, a: App, ax: App, b: App) {
    begin("psram.zero");
    let bad_fill = fill(&a.b).saturating_add(fill(&ax.b));
    let refused = psram::release_owner(0).is_none();
    stub_view();
    let zeroed = psram::release_owner(0);
    let (dc, du) = nonzero(&a.b);
    let (xc, xu) = nonzero(&ax.b);
    let stale = psram::grant(0, a.h).is_none();
    let _ = psram::set_quota(2, QUOTA);
    let c = get(2, Kind::Data, BLOCK);
    let reuse = match c {
        Ok(c) if c.b.off == a.b.off => mtrap::run_u(fnaddr(u_load), view_of(&c), cached(&c.b) + 64, 0).exit,
        _ => None,
    }
    .unwrap_or(usize::MAX);
    stub_view();
    let released = psram::release_owner(1) == Some(true) && psram::release_owner(2) == Some(true);
    let all_zero = dc + du + xc + xu == 0;
    record(
        t,
        "psram.zero",
        Some(bad_fill == 0 && refused && zeroed == Some(true) && all_zero && stale && reuse == 0 && released),
        format_args!(
            "A's data+exec filled, chip differs in {bad_fill}; release with A's view loaded refused {refused}, then \
             {zeroed:?}; non-zero data {dc}/{du} exec {xc}/{xu} (cached/uncached); stale handle refused {stale}; owner \
             2 got {:#x} (A {:#x}, B {:#x}), its U load reads {reuse:#x}; all released {released}",
            off(&c),
            a.b.off,
            b.b.off
        ),
    );
}

/// `li a0, 1; li a7, EXIT; ecall`: exit 1 from wherever it is loaded.
const EXIT1: [u32; 3] = [0x0010_0513, 0x0010_0893, 0x0000_0073];
/// The same with `li a0, 2`.
const EXIT2: [u32; 3] = [0x0020_0513, 0x0010_0893, 0x0000_0073];

/// Code in PSRAM (the exec arena), and stale code never survives a free. Owner 3 runs EXIT1 from its exec block
/// twice (the second run from a warm ICache); its blocks are released, owner 4 gets the same exec page, and running
/// it must fault illegal (the page is zeros; stale EXIT1 lines would exit). Then owner 4 loads EXIT2 there and it
/// must exit 2: `load_code`'s own ICache sync drops the zero line the faulting fetch left.
fn icache_row(t: &mut Tally) {
    begin("psram.icache");
    let _ = psram::set_quota(3, QUOTA).and(psram::set_quota(4, QUOTA));
    let run = |x: &App| {
        let Some(r) = psram::grant(x.owner, x.h) else { platform::exit(4) };
        crate::pmp::load(crate::pmp::checked(&[r]).plan());
        mtrap::run_u(r.base as usize, r.end as usize, 0, 0)
    };
    let e = get(3, Kind::Exec, PAGE);
    let first = e.map_or([None; 2], |e| {
        let ok = psram::load_code(e.owner, e.h, &EXIT1);
        [ok.then(|| run(&e).exit).flatten(), ok.then(|| run(&e).exit).flatten()]
    });
    stub_view();
    let released = psram::release_owner(3) == Some(true);
    let e2 = get(4, Kind::Exec, PAGE);
    let same = e.is_ok() && off(&e2) == off(&e);
    let code = e2.map_or(NO_FAULT, |x| code_of(&run(&x)));
    // Owner 4's own code: the exit value, or the fault code + 0x1000 so a failure names what happened.
    let second = e2.ok().filter(|x| psram::load_code(x.owner, x.h, &EXIT2)).map(|x| {
        let u = run(&x);
        u.exit.unwrap_or(0x1000 + code_of(&u) as usize)
    });
    stub_view();
    let _ = psram::release_owner(4);
    record(
        t,
        "psram.icache",
        Some(first == [Some(1); 2] && released && same && code == ILLEGAL && second == Some(2)),
        format_args!(
            "owner 3 ran its PSRAM code: exits {:?} {:?} (want 1 1); released {released}; owner 4 got the same page \
             {same}, running it: code {code} (want {ILLEGAL}, stale code would exit); its own code: {second:x?} \
             (want 2)",
            first[0], first[1]
        ),
    );
}
