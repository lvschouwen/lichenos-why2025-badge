//! The isolation harness. App B (heartbeat) runs for the whole session in slot 0. Each test loads app A (attacker)
//! into slot 1 with a test id, waits until A ends, checks how it ended, that B is still running and advanced, and
//! that the kernel canary is intact. Image-rejection tests run without starting any app.
use crate::loader::{self, LoadError};
use crate::sched::{End, Kernel, State};
use crate::{csr, kprintln, platform, trap};

static HEARTBEAT: &[u8] = include_bytes!(env!("LICHEN_APP_HEARTBEAT"));
static ATTACKER: &[u8] = include_bytes!(env!("LICHEN_APP_ATTACKER"));

const B: usize = 0;
const A: usize = 1;
const BUDGET: u32 = 50;

#[derive(Clone, Copy)]
enum Expect {
    Fault(usize),
    Exit0,
    Budget,
    Reject(LoadError2),
}
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum LoadError2 {
    Magic,
    Abi,
    Entry,
    Heap,
    TooBig,
}
#[derive(Clone, Copy)]
enum Target {
    None,
    KernelData,
    KernelText,
    OtherApp,
    Mmio,
    StolenHandle,
}
struct Case {
    id: usize,
    name: &'static str,
    target: Target,
    expect: Expect,
}
const fn c(id: usize, name: &'static str, target: Target, expect: Expect) -> Case {
    Case { id, name, target, expect }
}
const LOAD_FAULT: usize = 5;
const STORE_FAULT: usize = 7;
const INSN_FAULT: usize = 1;
const ILLEGAL: usize = 2;
static CASES: &[Case] = &[
    c(0, "read kernel memory", Target::KernelData, Expect::Fault(LOAD_FAULT)),
    c(1, "write kernel memory", Target::KernelData, Expect::Fault(STORE_FAULT)),
    c(2, "read other app memory", Target::OtherApp, Expect::Fault(LOAD_FAULT)),
    c(3, "write other app memory", Target::OtherApp, Expect::Fault(STORE_FAULT)),
    c(4, "execute own data", Target::None, Expect::Fault(INSN_FAULT)),
    c(5, "write own code", Target::None, Expect::Fault(STORE_FAULT)),
    c(6, "read peripheral MMIO", Target::Mmio, Expect::Fault(LOAD_FAULT)),
    c(7, "write pmpcfg0 (disable PMP)", Target::None, Expect::Fault(ILLEGAL)),
    c(8, "mret (privileged)", Target::None, Expect::Fault(ILLEGAL)),
    c(9, "illegal instruction", Target::None, Expect::Fault(ILLEGAL)),
    c(10, "invalid syscall arguments", Target::None, Expect::Exit0),
    c(11, "forged, stale and stolen handles", Target::StolenHandle, Expect::Exit0),
    c(12, "memory quota exhaustion", Target::None, Expect::Exit0),
    c(13, "endless loop (CPU budget)", Target::None, Expect::Budget),
    c(14, "jump into kernel code", Target::KernelText, Expect::Fault(INSN_FAULT)),
    c(15, "write mtvec (hijack traps)", Target::None, Expect::Fault(ILLEGAL)),
    c(100, "image: bad magic", Target::None, Expect::Reject(LoadError2::Magic)),
    c(101, "image: wrong ABI version", Target::None, Expect::Reject(LoadError2::Abi)),
    c(102, "image: entry outside image", Target::None, Expect::Reject(LoadError2::Entry)),
    c(103, "image: heap outside data region", Target::None, Expect::Reject(LoadError2::Heap)),
    c(104, "image: larger than its slot", Target::None, Expect::Reject(LoadError2::TooBig)),
];

pub struct Harness {
    next: usize,
    running: Option<usize>,
    /// A finished attacker waiting for proof that B kept running: (case, how A ended, B's syscall count then).
    judging: Option<(usize, End, u32)>,
    pass: u32,
    fail: u32,
}
impl Harness {
    pub const fn new() -> Self {
        Self { next: 0, running: None, judging: None, pass: 0, fail: 0 }
    }
}

fn kcanary() -> u32 {
    // SAFETY: plain read of a kernel static; apps cannot hold references to it.
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(crate::KCANARY)) }
}

pub fn boot(k: &mut Kernel) -> ! {
    csr::set_mtvec(trap::trap_entry as *const () as usize);
    let s = platform::slot(B);
    if let Err(e) = loader::load(k, B, "B", HEARTBEAT, s, [0; 4], None) {
        kprintln!("boot: heartbeat image refused: {e:?}");
        platform::exit(6);
    }
    kprintln!("boot: B loaded ({} bytes) at text {:#x} data {:#x}, A image {} bytes", HEARTBEAT.len(), s.text, s.data, ATTACKER.len());
    platform::timer_init();
    csr::enable_timer_irq();
    k.current = B;
    crate::pmp::apply(s.text, s.data, s.len);
    csr::mstatus_return_to_user();
    // SAFETY: task B's context was set up by the loader; mstatus.MPP = U, PMP holds B's view, mtvec points at
    // trap_entry, and mscratch is loaded by enter_user. Control comes back only through trap_entry.
    unsafe { trap::enter_user(&mut k.tasks[B].ctx) }
}

fn record(k: &mut Kernel, case: &Case, ok: bool, detail: core::fmt::Arguments) {
    if ok {
        k.harness.pass += 1;
    } else {
        k.harness.fail += 1;
    }
    kprintln!("TEST {:3} {:34} {} ({})", case.id, case.name, if ok { "PASS" } else { "FAIL" }, detail);
}

fn bad_image(kind: LoadError2, s: &crate::sched::Slot, buf: &mut [u8; 64]) -> usize {
    let words: [usize; 5] = [
        if kind == LoadError2::Magic { 0x4B4F_4F42 } else { lichen_abi::IMAGE_MAGIC as usize },
        if kind == LoadError2::Abi { 99 } else { lichen_abi::ABI_VERSION as usize },
        if kind == LoadError2::Entry { s.text + 0x10_0000 } else { s.text + 20 },
        if kind == LoadError2::Heap { s.data + s.len } else { s.data },
        s.text + buf.len(),
    ];
    for (i, w) in words.iter().enumerate() {
        buf[4 * i..4 * i + 4].copy_from_slice(&(*w as u32).to_le_bytes());
    }
    buf.len()
}

/// Start the next case; image cases are judged on the spot.
fn start_next(k: &mut Kernel) {
    while k.harness.next < CASES.len() {
        let case = &CASES[k.harness.next];
        k.harness.next += 1;
        let slot = platform::slot(A);
        if let Expect::Reject(kind) = case.expect {
            let mut buf = [0u8; 64];
            let n = bad_image(kind, &slot, &mut buf);
            let r = if kind == LoadError2::TooBig {
                let big = crate::sched::Slot { len: 16, ..slot };
                loader::validate(&buf[..n], &big).map(|_| ())
            } else {
                loader::load(k, A, "A", &buf[..n], slot, [0; 4], Some(BUDGET))
            };
            let want = match kind {
                LoadError2::Magic => LoadError::Magic,
                LoadError2::Abi => LoadError::Abi,
                LoadError2::Entry => LoadError::Entry,
                LoadError2::Heap => LoadError::Heap,
                LoadError2::TooBig => LoadError::TooBig,
            };
            let ok = r.as_ref().err() == Some(&want) && k.tasks[A].state != State::Ready;
            record(k, case, ok, format_args!("loader said {r:?}"));
            continue;
        }
        let bdata = k.tasks[B].slot.data;
        let a1 = match case.target {
            Target::None => 0,
            Target::KernelData => core::ptr::addr_of!(crate::KCANARY) as usize,
            Target::KernelText => crate::platform::init as *const () as usize,
            Target::OtherApp => bdata + 64,
            Target::Mmio => platform::UART_BASE,
            Target::StolenHandle => k.handles.first_of(B).unwrap_or(0),
        };
        let args = [case.id, a1, bdata + 64, core::ptr::addr_of!(crate::KCANARY) as usize];
        match loader::load(k, A, "A", ATTACKER, slot, args, Some(BUDGET)) {
            Ok(()) => {
                k.harness.running = Some(k.harness.next - 1);
                return;
            }
            Err(e) => record(k, case, false, format_args!("attacker image refused: {e:?}")),
        }
    }
    k.harness.running = None;
}

/// Called after every trap: judge a finished attacker and start the next case.
pub fn poll(k: &mut Kernel) {
    if k.tasks[B].state != State::Ready {
        kprintln!("B is no longer running: {:?}", k.tasks[B].end);
        finish(k);
    }
    if k.harness.running.is_none() && k.harness.next == 0 {
        start_next(k);
        return;
    }
    if let Some(ci) = k.harness.running {
        if k.tasks[A].state == State::Ended {
            k.harness.judging = Some((ci, k.tasks[A].end.unwrap(), k.tasks[B].syscalls));
            k.harness.running = None;
            k.tasks[A].state = State::Free;
        }
        return;
    }
    // B must make at least one more system call after A ended: proof it survived, not just that it is marked Ready.
    let Some((ci, end, b_then)) = k.harness.judging else { return };
    if k.tasks[B].syscalls <= b_then {
        return;
    }
    k.harness.judging = None;
    let case = &CASES[ci];
    let as_expected = match (case.expect, end) {
        (Expect::Fault(c), End::Fault { cause, .. }) => c == cause,
        (Expect::Exit0, End::Exit(0)) => true,
        (Expect::Budget, End::Budget) => true,
        _ => false,
    };
    let canary_ok = kcanary() == crate::KCANARY_VALUE;
    record(k, case, as_expected && canary_ok, format_args!("{end:x?}, B ran on, kernel canary {canary_ok}"));
    start_next(k);
    if k.harness.running.is_none() {
        finish(k);
    }
}

pub fn finish(k: &mut Kernel) -> ! {
    let b = k.tasks[B].state == State::Ready;
    kprintln!(
        "SUMMARY: {} pass, {} fail, B {} after {} syscalls, {} ticks",
        k.harness.pass,
        k.harness.fail,
        if b { "still running" } else { "DEAD" },
        k.tasks[B].syscalls,
        k.ticks
    );
    platform::exit(if k.harness.fail == 0 && b { 0 } else { 1 })
}
