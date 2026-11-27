//! Tasks, the trap dispatcher and a round-robin scheduler. App identity is the index of the task that trapped,
//! never a value the app passes.
use crate::global::Global;
use crate::handles::Handles;
use crate::trap::Context;
use crate::{kprintln, platform, pmp, syscall, tests};

pub const MAX_TASKS: usize = 2;

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

pub struct Task {
    pub state: State,
    pub ctx: Context,
    pub name: &'static str,
    pub slot: Slot,
    pub heap_start: usize,
    pub brk: usize,
    /// CPU budget in timer ticks; None = unlimited.
    pub budget: Option<u32>,
    pub ticks: u32,
    pub syscalls: u32,
    pub end: Option<End>,
}
impl Task {
    pub const EMPTY: Self = Self {
        state: State::Free,
        ctx: Context::ZERO,
        name: "",
        slot: Slot { text: 0, data: 0, len: 0 },
        heap_start: 0,
        brk: 0,
        budget: None,
        ticks: 0,
        syscalls: 0,
        end: None,
    };
}

pub struct Kernel {
    pub tasks: [Task; MAX_TASKS],
    pub current: usize,
    pub ticks: u64,
    pub handles: Handles,
    pub harness: tests::Harness,
}

pub static KERNEL: Global<Kernel> = Global::new(Kernel {
    tasks: [Task::EMPTY; MAX_TASKS],
    current: 0,
    ticks: 0,
    handles: Handles::new(),
    harness: tests::Harness::new(),
});

pub fn end_task(k: &mut Kernel, i: usize, why: End) {
    let t = &mut k.tasks[i];
    t.state = State::Ended;
    t.end = Some(why);
    k.handles.release_owner(i);
}

pub fn on_trap(k: &mut Kernel, ctx: *mut Context, mcause: usize, mtval: usize) -> *mut Context {
    let cur = k.current;
    if !core::ptr::eq(ctx, &k.tasks[cur].ctx) {
        kprintln!("trap: context does not belong to the current task");
        platform::exit(5);
    }
    let irq = mcause & (1 << 31) != 0;
    let code = mcause & 0x7FFF_FFFF;
    let mut switch = false;
    if irq {
        if platform::is_timer_irq(code) {
            platform::timer_rearm();
            k.ticks += 1;
            let t = &mut k.tasks[cur];
            t.ticks += 1;
            if t.budget.is_some_and(|b| t.ticks > b) {
                end_task(k, cur, End::Budget);
            }
            switch = true;
        } else {
            kprintln!("trap: unexpected interrupt {code}");
        }
    } else if code == 8 {
        k.tasks[cur].ctx.pc += 4;
        k.tasks[cur].syscalls += 1;
        switch = syscall::dispatch(k, cur);
    } else {
        let pc = k.tasks[cur].ctx.pc;
        end_task(k, cur, End::Fault { cause: code, tval: mtval, pc });
    }
    tests::poll(k);
    if switch || k.tasks[cur].state != State::Ready {
        schedule(k)
    } else {
        &mut k.tasks[cur].ctx
    }
}

/// Pick the next Ready task after the current one and load its PMP view.
pub fn schedule(k: &mut Kernel) -> *mut Context {
    for step in 1..=MAX_TASKS {
        let i = (k.current + step) % MAX_TASKS;
        if k.tasks[i].state == State::Ready {
            k.current = i;
            let s = k.tasks[i].slot;
            pmp::apply(s.text, s.data, s.len);
            return &mut k.tasks[i].ctx;
        }
    }
    kprintln!("sched: nothing runnable");
    tests::finish(k)
}
