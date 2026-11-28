//! The process model (#617): a process table per hart, the trap dispatcher, and a round-robin scheduler that switches
//! on the tick and reloads each process's precomputed PMP view. A process's identity is the index of the PCB that
//! trapped, never a value it passes.
//!
//! Each hart has its own `Kernel` (table, tick count, handles), touched only by that hart's trap handler, with one
//! hand-off: core 0 (the services core) fills core 1's table before it sends OP_PROCS, and reads it back after core 1
//! replied (`procs`). The mailbox push and pop (release, acquire) order both. Who judges the processes is the
//! supervisor: the #607/#613 isolation harness on core 0 (`tests`), the process stage on core 1 (`procs`).
use crate::global::Global;
use crate::handles::Handles;
use crate::trap::Context;
use crate::{csr, kprintln, platform, pmp, syscall, tests};
use core::mem::MaybeUninit;
use lichen_kcore::account::{over_cycles, reclaim_ranges, Meter};
use lichen_kcore::sched::{over_budget, pick_next};
use lichen_kcore::view::View;

/// Process slots per hart: two apps and an idle process.
pub const MAX_PROCS: usize = 3;
const HARTS: usize = 2;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
    Free,
    Ready,
    Ended,
}
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum End {
    Exit(isize),
    Fault { cause: usize, tval: usize, pc: usize },
    Budget,
}

pub use lichen_kcore::image::Slot;

/// The process control block.
pub struct Pcb {
    pub state: State,
    pub ctx: Context,
    pub name: &'static str,
    pub slot: Slot,
    /// The PMP view, planned and checked once at load; every switch to this process loads exactly this.
    pub view: Option<View>,
    pub heap_start: usize,
    pub brk: usize,
    /// CPU budget in timer ticks; None = unlimited.
    pub budget: Option<u32>,
    pub ticks: u32,
    pub syscalls: u32,
    pub end: Option<End>,
    /// CPU time in cycles (#618): charged at every trap entry from the point the process was resumed.
    pub meter: Meter,
    /// End the process once its meter is past this many cycles; None = unlimited. Unlike the tick budget, yielding
    /// before the tick does not dodge it.
    pub cycle_budget: Option<u64>,
    /// Bytes of the loaded image in the text region (what `reclaim` zeroes there).
    pub image_len: usize,
    /// The hart's tick count when the process ended.
    pub ended_tick: u64,
    /// mcycle spent giving the process's memory back at its end.
    pub reclaim_cycles: u32,
}
impl Pcb {
    pub const EMPTY: Self = Self {
        state: State::Free,
        ctx: Context::ZERO,
        name: "",
        slot: Slot { text: 0, data: 0, len: 0 },
        view: None,
        heap_start: 0,
        brk: 0,
        budget: None,
        ticks: 0,
        syscalls: 0,
        end: None,
        meter: Meter::new(),
        cycle_budget: None,
        image_len: 0,
        ended_tick: 0,
        reclaim_cycles: 0,
    };
}

/// Who judges the processes after every trap, and what happens when nothing can run.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Supervisor {
    Harness,
    #[cfg(feature = "esp32p4")]
    Procs,
}

/// Cycle samples (bss): the first `CAP` of a run are kept.
pub struct Samples {
    pub buf: [u32; Samples::CAP],
    pub n: usize,
}
impl Samples {
    pub const CAP: usize = 128;
    const fn new() -> Self {
        Self { buf: [0; Self::CAP], n: 0 }
    }
    pub fn push(&mut self, v: u32) {
        if let Some(s) = self.buf.get_mut(self.n) {
            *s = v;
            self.n += 1;
        }
    }
}

/// The switch cost (#617): tick-driven switches only.
pub struct SwitchPerf {
    /// mcycle from the trap entry stamp to the end of trap_handler.
    pub switch: Samples,
    /// mcycle around `pmp::switch` alone, for the same switches.
    pub pmp: Samples,
    /// PMP registers written by the last tick-driven switch.
    pub pmp_writes: u32,
    /// (mcycle, registers written) of the last `pmp::switch`, whatever caused it.
    pub last_pmp: (u32, u32),
    /// Set by `on_trap` for trap_handler: this trap was a tick that switched processes.
    pub pending: bool,
    /// The last tick-driven switch, mcycle from the entry stamp at: on_trap's start, the tick handled (rearm and
    /// budget), the supervisor done, the next process scheduled (PMP reload included). Printed as a breakdown.
    pub marks: [u32; 4],
}

/// One hart's process kernel. The switch samples live apart, in `PERF`.
pub struct Kernel {
    pub procs: [Pcb; MAX_PROCS],
    pub current: usize,
    /// The idle process: runs only when nothing else can, never ends on a budget.
    pub idle: Option<usize>,
    pub ticks: u64,
    pub handles: Handles,
    pub sup: Supervisor,
    pub harness: tests::Harness,
    /// The whole run ends at this mtime (the supervisor checks it after every trap), so a run that keeps spinning is
    /// bounded even when no process has a budget (#612 review L-b).
    #[cfg_attr(not(feature = "esp32p4"), allow(dead_code))]
    pub deadline: Option<u64>,
    /// An ended process gives its memory and PMP view back at once (`end_proc`). Off for the #607/#613 harness and
    /// the #617 spinner stages, whose rows read the dead processes' memory afterwards.
    pub reclaim: bool,
    /// The kernel's own time: trap entry to the resume of the next process, summed over every trap. With the meters of
    /// the processes it adds up to the stage's wall clock (`stage_cycles`).
    pub kernel_time: Meter,
    /// mcycle the stage took on its hart (set by `procs::stage`): the wall clock the meters add up to.
    #[cfg_attr(not(feature = "esp32p4"), allow(dead_code))]
    pub stage_cycles: u64,
    /// Tick-driven switches, [from][to].
    pub preempts: [[u32; MAX_PROCS]; MAX_PROCS],
}
impl Kernel {
    const fn new() -> Self {
        Self {
            procs: [Pcb::EMPTY; MAX_PROCS],
            current: 0,
            idle: None,
            ticks: 0,
            handles: Handles::new(),
            sup: Supervisor::Harness,
            harness: tests::Harness::new(),
            deadline: None,
            reclaim: false,
            kernel_time: Meter::new(),
            stage_cycles: 0,
            preempts: [[0; MAX_PROCS]; MAX_PROCS],
        }
    }
    /// Back to the boot state (core 0 before it fills core 1's table).
    #[cfg(feature = "esp32p4")]
    pub fn reset(&mut self) {
        *self = Self::new();
    }
}

/// Uninitialised in .bss (a const-initialised table would sit in .data, since the PCBs' None values are not all-zero
/// bytes): `init` writes both before anything else uses them.
static KERNELS: [Global<MaybeUninit<Kernel>>; HARTS] = [const { Global::new(MaybeUninit::uninit()) }; HARTS];

/// Write both harts' tables. Called once by kmain on core 0, before the first `this` / `of` and before core 1 starts.
pub fn init() {
    for k in &KERNELS {
        // SAFETY: called once at boot on core 0, before any reference into KERNELS exists and before core 1 runs.
        unsafe { k.get() }.write(Kernel::new());
    }
}
/// All zero, so in .bss: 1 KB per hart that the loaded image does not carry. Same ownership rules as KERNELS.
static PERF: [Global<SwitchPerf>; HARTS] = [const {
    Global::new(SwitchPerf {
        switch: Samples::new(),
        pmp: Samples::new(),
        pmp_writes: 0,
        last_pmp: (0, 0),
        pending: false,
        marks: [0; 4],
    })
}; HARTS];

/// This hart's switch samples.
///
/// # Safety
/// As `this`, for PERF: no other live reference to this hart's instance.
pub unsafe fn perf() -> &'static mut SwitchPerf {
    PERF[hart()].get()
}

/// Hart `h`'s switch samples, from another hart: after the OP_PROCS reply only (see `of`).
///
/// # Safety
/// As `of`.
#[cfg(feature = "esp32p4")]
pub unsafe fn perf_of(h: usize) -> &'static mut SwitchPerf {
    PERF[h & 1].get()
}

fn hart() -> usize {
    #[cfg(feature = "esp32p4")]
    return crate::smp::hartid() & 1;
    #[cfg(not(feature = "esp32p4"))]
    0
}

/// This hart's Kernel.
///
/// # Safety
/// As `Global::get`: no other live reference to this hart's Kernel; `init` has run.
pub unsafe fn this() -> &'static mut Kernel {
    // SAFETY (init): kmain wrote both tables before any caller runs.
    KERNELS[hart()].get().assume_init_mut()
}

/// Hart `h`'s Kernel, from another hart: the OP_PROCS hand-off only (see the module doc).
///
/// # Safety
/// Hart `h` must not use its Kernel meanwhile (core 1 waits in its mailbox loop), and no other reference to it exists.
#[cfg(feature = "esp32p4")]
pub unsafe fn of(h: usize) -> &'static mut Kernel {
    // SAFETY (init): kmain wrote both tables before any caller runs.
    KERNELS[h & 1].get().assume_init_mut()
}

/// End process `i`: it never runs again, its handles go (their generation moves on, so copies fail). With `k.reclaim`
/// its memory is zeroed and its PMP view dropped as well, so nothing of it is left for the next image in the slot.
pub fn end_proc(k: &mut Kernel, i: usize, why: End) {
    let p = &mut k.procs[i];
    p.state = State::Ended;
    p.end = Some(why);
    p.ended_tick = k.ticks;
    k.handles.release_owner(i);
    if k.reclaim && Some(i) != k.idle {
        reclaim(&mut k.procs[i]);
    }
}

/// Zero what the process held in its slot and forget its PMP view. The kernel is in M mode, so the process's own view
/// (still loaded until the next switch) does not stand in the way. The idle process is never reclaimed: its "slot" is
/// the kernel's U stub page.
fn reclaim(p: &mut Pcb) {
    let c0 = csr::mcycle() as u32;
    for (addr, len) in reclaim_ranges(&p.slot, p.image_len) {
        // SAFETY: the ranges lie inside this process's slot (reclaim_ranges, host-tested), which is NOLOAD L2MEM
        // reserved for apps; the process has ended and the kernel is the only one running on this hart, and no other
        // hart runs it.
        unsafe { core::ptr::write_bytes(addr as *mut u8, 0, len) };
    }
    p.view = None;
    p.reclaim_cycles = (csr::mcycle() as u32).wrapping_sub(c0);
}

pub fn on_trap(k: &mut Kernel, ctx: *mut Context, mcause: usize, mtval: usize) -> *mut Context {
    let mut marks = [csr::mcycle() as u32, 0, 0, 0];
    let cur = k.current;
    if !core::ptr::eq(ctx, &k.procs[cur].ctx) {
        kprintln!("trap: context does not belong to the current process");
        platform::exit(5);
    }
    // Charge the span that just ended (resume stamp to this trap's entry stamp) to the process that ran it.
    let entry = k.procs[cur].ctx.entry_cycle as u32;
    let spent = k.procs[cur].meter.charge(entry);
    k.kernel_time.resume(entry);
    // CLIC layout on the P4 (code = bits 11:0); a plain mcause decodes the same for every standard code.
    let cause = lichen_kcore::trap::decode(mcause as u32);
    let (irq, code) = (cause.interrupt, cause.code as usize);
    let mut switch = false;
    let mut tick = false;
    if irq {
        if platform::is_timer_irq(code) {
            platform::timer_rearm();
            k.ticks += 1;
            let p = &mut k.procs[cur];
            p.ticks += 1;
            if Some(cur) != k.idle && over_budget(p.ticks, p.budget) {
                end_proc(k, cur, End::Budget);
            }
            switch = true;
            tick = true;
            marks[1] = csr::mcycle() as u32;
        } else {
            // Masked, or a level line (the P4 CLIC) would be taken again on every return and starve every process.
            kprintln!("trap: unexpected interrupt {code}, line masked");
            platform::mask_irq(code);
        }
    } else if code == 8 {
        k.procs[cur].ctx.pc += 4;
        k.procs[cur].syscalls += 1;
        switch = syscall::dispatch(k, cur);
    } else {
        let pc = k.procs[cur].ctx.pc;
        end_proc(k, cur, End::Fault { cause: code, tval: mtval, pc });
    }
    // After the trap's own outcome, so a fault or an exit keeps its cause; any trap can end a process that is over.
    if k.procs[cur].state == State::Ready && Some(cur) != k.idle && over_cycles(spent, k.procs[cur].cycle_budget) {
        end_proc(k, cur, End::Budget);
    }
    supervise(k);
    marks[2] = csr::mcycle() as u32;
    if !switch && k.procs[cur].state == State::Ready && keep_running(k, cur) {
        return &mut k.procs[cur].ctx;
    }
    let next = schedule(k);
    // A switch that ended a process zeroed its memory inside the trap: not a switch cost, so no sample.
    let ended = k.procs[cur].state != State::Ready;
    if tick && k.current != cur && !ended {
        k.preempts[cur][k.current] += 1;
        marks[3] = csr::mcycle() as u32;
        // SAFETY: this hart's samples; trap context, the only reference.
        let p = unsafe { perf() };
        p.pending = true;
        p.marks = marks.map(|m| m.wrapping_sub(entry));
        p.pmp.push(p.last_pmp.0);
        p.pmp_writes = p.last_pmp.1;
    }
    resume(k, k.current);
    next
}

/// The trap returns to the same process: its service time is the process's (see `Meter::keep_running`), so the budget is
/// checked again; false when that ended it (the caller schedules another process).
fn keep_running(k: &mut Kernel, cur: usize) -> bool {
    let total = k.procs[cur].meter.keep_running(csr::mcycle() as u32);
    if Some(cur) != k.idle && over_cycles(total, k.procs[cur].cycle_budget) {
        end_proc(k, cur, End::Budget);
        return false;
    }
    true
}

/// The kernel is done with this trap and hands the CPU to process `i`: the span since the trap entry is the kernel's,
/// the next one is the process's.
fn resume(k: &mut Kernel, i: usize) {
    let now = csr::mcycle() as u32;
    k.kernel_time.charge(now);
    k.procs[i].meter.resume(now);
}

fn supervise(k: &mut Kernel) {
    match k.sup {
        Supervisor::Harness => tests::poll(k),
        #[cfg(feature = "esp32p4")]
        Supervisor::Procs => crate::procs::poll(k),
    }
}

/// Load the current process's view (if the PMP does not hold it) and return its context: the first entry to U.
pub fn schedule_current(k: &mut Kernel) -> *mut Context {
    let i = k.current;
    load_view(k, i);
    k.procs[i].meter.resume(csr::mcycle() as u32);
    &mut k.procs[i].ctx
}

fn load_view(k: &mut Kernel, i: usize) {
    let Some(v) = k.procs[i].view.as_ref() else {
        kprintln!("sched: process {i} has no PMP view: halting");
        platform::exit(4)
    };
    let c0 = csr::mcycle() as u32;
    let writes = pmp::switch(v);
    let c = (csr::mcycle() as u32).wrapping_sub(c0);
    // SAFETY: this hart's samples; kernel context with MIE clear, the only reference.
    unsafe { perf() }.last_pmp = (c, writes);
}

/// Pick the next process (round robin, idle last) and load its PMP view. Only a change of process reloads the PMP.
pub fn schedule(k: &mut Kernel) -> *mut Context {
    let idle = k.idle.filter(|&i| k.procs[i].state == State::Ready);
    let Some(i) = pick_next(MAX_PROCS, k.current, idle, |i| k.procs[i].state == State::Ready) else {
        kprintln!("sched: nothing runnable");
        match k.sup {
            Supervisor::Harness => tests::finish(k),
            #[cfg(feature = "esp32p4")]
            Supervisor::Procs => crate::procs::finish(k),
        }
    };
    if i != k.current || !k.procs[i].view.as_ref().is_some_and(pmp::holds) {
        load_view(k, i);
    }
    k.current = i;
    &mut k.procs[i].ctx
}
