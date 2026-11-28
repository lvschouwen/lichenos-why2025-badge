//! The process stage (#617): U processes on core 1, services on core 0. Core 0 loads two spinner processes into the
//! app slots and an idle process on the U stub page, fills core 1's process table (`prepare`) and sends OP_PROCS.
//! Core 1 (`stage`) installs the process vector, runs them under its own 1 ms tick with each one's own PMP view, and
//! comes back to its mailbox loop when the stage is over (`task_leave`), so it can reply and keep answering.
//!
//! The spinners never yield: every switch between them is the tick. Each counts its rounds in its own data and asks
//! the kernel for the tick count (TICKS, which does not switch); at its tick it touches the other one's data (P0 loads,
//! P1 stores), which must fault and end only that process. Then the idle process (a U spin) runs until the stage ends.
//! A spinner that never faults ends on its budget; the whole stage on its deadline, so OP_PROCS always gets a reply.
//!
//! Core 1 prints nothing here: its console is raw and would interleave with core 0's rows. Core 0 judges the stage
//! from core 1's table after the reply (`bringup_procs`).
use crate::loader::{self, LoadError};
use crate::platform::{self, hw};
use crate::sched::{self, Kernel, Pcb, Slot, State, Supervisor};
use crate::trap::{self, Context};
use crate::{csr, pmp};
use lichen_abi::sys;
use lichen_kcore::image::HEADER_LEN;
use lichen_kcore::pmp::{Region, R, X};

/// The two spinners are processes 0 and 1 (app slots 0 and 1), the idle process is 2.
pub const APPS: usize = 2;
pub const IDLE: usize = 2;
/// Spinner i touches the other one's data at kernel tick UNTIL[i]: both run (alternating) until tick 30.
pub const UNTIL: [usize; APPS] = [30, 40];
/// Where in the other spinner's data the access goes (its round counter is at offset 0).
pub const TARGET_OFFSET: usize = 64;
/// CPU budget per spinner, in ticks: far above what UNTIL needs, so only a spinner that never faults reaches it.
pub const BUDGET: u32 = 200;
/// The stage ends once both spinners ended and idle ran this many ticks (OP_PROCS a[1] overrides it) ...
pub const IDLE_TICKS: u32 = 10;
/// ... or at this deadline, whatever happened (OP_PROCS a[0], in ms, overrides it).
const DEADLINE_US: u64 = 1_000_000;
/// A spinner that never touches the other one's data (its tick never comes): it can only end on its budget.
pub const NEVER: usize = usize::MAX;
/// `stage` reply a[0].
pub const DONE: u32 = 0;
pub const TIMED_OUT: u32 = 1;
pub const NOTHING_RUNNABLE: u32 = 2;
pub const NOT_PREPARED: u32 = 3;

const UPAGE: usize = 4096;
const IMG_MAX: usize = 128;

extern "C" {
    static _utext: u8;
    fn u_idle();
    static proc_spin_start: u8;
    static proc_spin_end: u8;
    static _stack1_top: u8;
}

/// Core 1's kernel stack (link-esp32p4.ld `.stack1`: 8 KB below `_stack1_top`); the stage's traps run on what is left
/// of it below `stage`'s frame.
const STACK1: usize = 8192;
const PAINT: u32 = 0x5A17_5A17;
/// Set on core 1 around `stage`'s task_run: `leave` refuses to task_leave outside it (review L5).
static IN_STAGE: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(false);
/// The idle ticks this stage waits for (core 1 only, set by `stage`).
static IDLE_GOAL: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(IDLE_TICKS);
/// mintstatus as `leave` found it, inside the trap that ends the stage (core 1 only).
static LEAVE_MINTSTATUS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

// The spinner, position independent (branches only), assembled into rodata and copied behind a header at load
// (`spin_image`). a0 = the tick to stop at, a1 = the other process's address, a2 = 1: store there, 0: load; a3 = own
// round counter. Exit 100 means the access did NOT fault.
core::arch::global_asm!(
    ".pushsection .rodata.proc_spin, \"a\"",
    ".option push",
    ".option norvc",
    ".balign 4",
    ".global proc_spin_start",
    "proc_spin_start:",
    "    mv s0, a0",
    "5:  li t1, {spin}",
    "6:  addi t1, t1, -1",
    "    bnez t1, 6b",
    "    lw t0, 0(a3)",
    "    addi t0, t0, 1",
    "    sw t0, 0(a3)",
    "    li a7, {ticks}",
    "    ecall",
    "    bltu a0, s0, 5b",
    "    bnez a2, 7f",
    "    lw t0, 0(a1)",
    "    j 8f",
    "7:  sw a2, 0(a1)",
    "8:  li a0, 100",
    "    li a7, {exit}",
    "    ecall",
    ".global proc_spin_end",
    "proc_spin_end:",
    ".option pop",
    ".popsection",
    // The idle process: on the U stub page, which is all its view grants (R+X). It spins: a U wfi is an illegal
    // instruction on the P4 (bench, #617: `u_wfi` through OP_URUN on core 1 faults with code 2, mtval = the wfi word,
    // row proc.u-wfi), although mstatus.TW reads back 0.
    ".pushsection .utext, \"ax\"",
    ".balign 4",
    ".global u_idle",
    "u_idle:",
    "    j u_idle",
    ".global u_wfi",
    "u_wfi:",
    "    wfi",
    "    li a7, 1",
    "    ecall",
    ".popsection",
    spin = const 256,
    ticks = const sys::TICKS,
    exit = const sys::EXIT,
);

fn sym(s: &u8) -> usize {
    s as *const u8 as usize
}

/// A spinner image for slot `s`: the loader's header (magic, ABI, entry, heap start, image end), then the code.
fn spin_image<'a>(s: &Slot, buf: &'a mut [u8; IMG_MAX]) -> &'a [u8] {
    // SAFETY: linker symbols around the spinner's bytes in rodata (above); only their addresses are taken.
    let (start, end) = unsafe { (sym(&proc_spin_start), sym(&proc_spin_end)) };
    let n = HEADER_LEN + (end - start);
    if n > IMG_MAX {
        crate::kprintln!("procs: spinner image {n} B does not fit {IMG_MAX}: halting");
        platform::exit(2);
    }
    let words = [
        lichen_abi::IMAGE_MAGIC,
        lichen_abi::ABI_VERSION,
        (s.text + HEADER_LEN) as u32,
        s.data as u32,
        (s.text + n) as u32,
    ];
    for (i, w) in words.iter().enumerate() {
        buf[4 * i..4 * i + 4].copy_from_slice(&w.to_le_bytes());
    }
    // SAFETY: start..end is the spinner's code in the loaded image's rodata, n - HEADER_LEN bytes.
    let code = unsafe { core::slice::from_raw_parts(start as *const u8, end - start) };
    buf[HEADER_LEN..n].copy_from_slice(code);
    &buf[..n]
}

/// The address spinner `i` touches: inside the other spinner's data region.
pub fn target(i: usize) -> usize {
    platform::slot(1 - i).data + TARGET_OFFSET
}

/// Core 0: fill core 1's process table. Core 1 must be waiting in its mailbox loop (it does not touch its table until
/// OP_PROCS), and its parked U stubs must not run from the slots (they run from the stub page).
///
/// The views are planned here at core 0's PMP granularity (`pmp::gran()` of hart 0). Both harts measured 128 B at
/// boot (pmp.report), and `pmp::switch` on core 1 halts fail-closed if its own granularity differs (review L4).
pub fn prepare(until: [usize; APPS], budget: [u32; APPS]) -> Result<(), LoadError> {
    // SAFETY: core 1 does not use its Kernel until OP_PROCS (module doc); this is the only reference to it.
    let k = unsafe { sched::of(1) };
    k.reset();
    k.sup = Supervisor::Procs;
    let mut buf = [0u8; IMG_MAX];
    for (i, name) in ["P0", "P1"].into_iter().enumerate() {
        let s = platform::slot(i);
        let img = spin_image(&s, &mut buf);
        loader::load(k, i, name, img, s, [until[i], target(i), i, s.data], Some(budget[i]))?;
    }
    // SAFETY: linker symbols; only their addresses are used.
    let (utext, idle_pc) = unsafe { (sym(&_utext), u_idle as *const () as usize) };
    let mut ctx = Context::ZERO;
    ctx.pc = idle_pc;
    let view = pmp::checked(&[Region { base: utext as u32, end: (utext + UPAGE) as u64, perm: R | X }]);
    // The idle process has no data: its slot names the stub page twice, so a stray syscall pointer can only name it.
    let slot = Slot { text: utext, data: utext, len: UPAGE };
    k.procs[IDLE] = Pcb { state: State::Ready, ctx, name: "idle", slot, view: Some(view), ..Pcb::EMPTY };
    k.idle = Some(IDLE);
    k.current = 0;
    Ok(())
}

fn sp() -> usize {
    let v: usize;
    // SAFETY: reads the stack pointer.
    unsafe { core::arch::asm!("mv {0}, sp", out(reg) v, options(nomem, nostack)) };
    v
}

/// Paint core 1's free stack from its bottom to 256 bytes below the current sp. Returns (bottom, painted top).
fn paint() -> (usize, usize) {
    // SAFETY: a linker symbol; only its address is used.
    let bottom = unsafe { sym(&_stack1_top) } - STACK1;
    let top = (sp() - 256) & !3;
    for a in (bottom..top).step_by(4) {
        // SAFETY: core 1's own stack below its live frames (256 bytes of margin for this loop); nothing else uses it.
        unsafe { core::ptr::write_volatile(a as *mut u32, PAINT) };
    }
    (bottom, top)
}

/// Bytes below the painted top that the stage used (the deepest word no longer painted).
fn high_water(bottom: usize, top: usize) -> u32 {
    // SAFETY: as `paint`; the stage is over, nothing runs on this part of the stack now.
    let first = (bottom..top).step_by(4).find(|&a| unsafe { core::ptr::read_volatile(a as *const u32) } != PAINT);
    first.map_or(0, |a| (top - a) as u32)
}

/// Core 1, on OP_PROCS: run the prepared processes until the stage ends, at the latest after `deadline_ms` (0: 1 s);
/// it is done once both spinners ended and idle ran `idle_ticks` (0: IDLE_TICKS). Reply a = [outcome, ticks,
/// mintstatus after the stage, mintstatus at the leave inside its last trap, stack bytes used below `stage`'s frame,
/// bit 0: the PMP matched its shadow at the start, bit 1: at the end].
pub fn stage(deadline_ms: u32, idle_ticks: u32) -> [u32; 6] {
    // Entered from the mailbox loop at level 0: a raised CLIC level (mintstatus.mil) or a nonzero mcause.mpil (bits
    // 23:16, what the first mret would restore) would mask the tick for the whole stage (review L1). Read only.
    if hw::trap::mintstatus() != 0 || (hw::trap::mcause() >> 16) & 0xFF != 0 {
        return [NOT_PREPARED, 0, 0, 0, 0, 0];
    }
    crate::smp::uninhibit_mcycle();
    let (bottom, top) = paint();
    let (mtvec, mscratch) = (hw::trap::mtvec() as usize, csr::mscratch());
    let ctx = {
        // SAFETY: core 0 handed this table over with OP_PROCS (it reads it again only after the reply); no other
        // reference exists on this hart. The borrow ends before task_run, whose traps take their own.
        let k = unsafe { sched::this() };
        if k.sup != Supervisor::Procs || k.procs[0].state != State::Ready {
            return [NOT_PREPARED, 0, 0, 0, 0, 0];
        }
        let us = if deadline_ms == 0 { DEADLINE_US } else { deadline_ms as u64 * 1000 };
        k.deadline = Some(hw::clint::mtime() + crate::timebase::us_to_ticks(us));
        let goal = if idle_ticks == 0 { IDLE_TICKS } else { idle_ticks };
        IDLE_GOAL.store(goal, core::sync::atomic::Ordering::Relaxed);
        // mscratch = 0 before the vector: MIE is clear here (the mailbox loop never sets it), so nothing traps
        // in between, and the first trap from U finds the process context task_restore put there.
        csr::clear_mscratch();
        csr::set_mtvec(trap::task_trap_entry as *const () as usize | platform::MTVEC_MODE);
        platform::timer_init();
        let ctx = sched::schedule_current(k);
        csr::mstatus_return_to_user();
        trap::check_return(k, ctx);
        (ctx, pmp::shadow_matches())
    };
    let (ctx, shadow_start) = ctx;
    // SAFETY: ctx is process 0's context, set up by the loader; MPP = U, its view is loaded and checked, the vector
    // and the tick are this hart's. Control comes back here only through task_leave (`leave`).
    IN_STAGE.store(true, core::sync::atomic::Ordering::Relaxed);
    let outcome = unsafe { trap::task_run(ctx) } as u32;
    IN_STAGE.store(false, core::sync::atomic::Ordering::Relaxed);
    platform::timer_stop();
    csr::set_mtvec(mtvec);
    csr::set_mscratch(mscratch);
    let shadow_end = pmp::shadow_matches();
    let mintstatus = hw::trap::mintstatus();
    let at_leave = LEAVE_MINTSTATUS.load(core::sync::atomic::Ordering::Relaxed);
    // SAFETY: the stage is over, no trap handler borrow is live (task_leave dropped those frames).
    let k = unsafe { sched::this() };
    let shadow = shadow_start as u32 | (shadow_end as u32) << 1;
    [outcome, k.ticks as u32, mintstatus, at_leave, high_water(bottom, top), shadow]
}

/// The stage's supervisor, after every trap on core 1: end it when its work is done or its deadline passed.
pub fn poll(k: &mut Kernel) {
    let apps_done = k.procs[..APPS].iter().all(|p| p.state == State::Ended);
    if apps_done && k.procs[IDLE].ticks >= IDLE_GOAL.load(core::sync::atomic::Ordering::Relaxed) {
        leave(DONE);
    }
    if lichen_kcore::sched::past_deadline(hw::clint::mtime(), k.deadline) {
        leave(TIMED_OUT);
    }
}

/// Nothing can run, not even idle (it ended): end the stage.
pub fn finish(_k: &mut Kernel) -> ! {
    leave(NOTHING_RUNNABLE)
}

fn leave(outcome: u32) -> ! {
    if !IN_STAGE.load(core::sync::atomic::Ordering::Relaxed) || crate::smp::hartid() != 1 {
        platform::enter_fatal();
        crate::kprintln!("procs: stage end outside core 1's stage: halting");
        platform::exit(5);
    }
    LEAVE_MINTSTATUS.store(hw::trap::mintstatus(), core::sync::atomic::Ordering::Relaxed);
    // SAFETY: on core 1 inside `stage`'s task_run (IN_STAGE, checked above). Only `stage` installs task_trap_entry
    // on core 1, and task_run fills this hart's task_kctx before the first entry to U, so every trap that reaches
    // here came from that run and task_kctx[1] holds `stage`'s frame. The caller's borrow of the Kernel is not used
    // again.
    unsafe { trap::task_leave(outcome as usize) }
}
