//! The isolation harness. App B (heartbeat) runs for the whole session in slot 0. Each test loads app A (attacker)
//! into slot 1 with a test id, waits until A ends, checks how it ended, that B is still running and advanced, and
//! that the kernel canary is intact. Image-rejection tests run without starting any app.
//!
//! Both platforms run the same cases and the same judging. qemu-virt (#607) prints `TEST` lines and a `SUMMARY`; the
//! ESP32-P4 (#613) prints one bench `CHECK iso.<id>` row per case and ends the bring-up run (`bringup_iso`).
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
    /// The fault code, and where it must have happened.
    Fault(usize, At),
    Exit0,
    Budget,
    Reject(LoadError2),
    /// A different image in A's slot (`reload_image`): it must run its own code, not stale cached attacker code.
    #[cfg_attr(not(feature = "esp32p4"), allow(dead_code))]
    Reload,
}
/// Where a fault must point, beyond its code: a fault elsewhere is not the protection under test.
#[derive(Clone, Copy)]
enum At {
    /// mtval is the target address, and the faulting pc is in A's text.
    Target,
    /// A jump to the target: mtval and the pc are both the target.
    Jump,
    /// The faulting pc is in A's text (illegal instructions; mtval is the instruction or 0, chip-dependent).
    InText,
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
    /// A's own heap start (data, never executable): where case 4 writes its instruction.
    OwnData,
    /// A's own entry (text, never writable).
    OwnText,
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
const RELOAD_EXIT: isize = 0x613;
static CASES: &[Case] = &[
    c(0, "read kernel memory", Target::KernelData, Expect::Fault(LOAD_FAULT, At::Target)),
    c(1, "write kernel memory", Target::KernelData, Expect::Fault(STORE_FAULT, At::Target)),
    c(2, "read other app memory", Target::OtherApp, Expect::Fault(LOAD_FAULT, At::Target)),
    c(3, "write other app memory", Target::OtherApp, Expect::Fault(STORE_FAULT, At::Target)),
    c(4, "execute own data", Target::OwnData, Expect::Fault(INSN_FAULT, At::Jump)),
    c(5, "write own code", Target::OwnText, Expect::Fault(STORE_FAULT, At::Target)),
    c(6, "read peripheral MMIO", Target::Mmio, Expect::Fault(LOAD_FAULT, At::Target)),
    c(7, "write pmpcfg0 (disable PMP)", Target::None, Expect::Fault(ILLEGAL, At::InText)),
    c(8, "mret (privileged)", Target::None, Expect::Fault(ILLEGAL, At::InText)),
    c(9, "illegal instruction", Target::None, Expect::Fault(ILLEGAL, At::InText)),
    c(10, "invalid syscall arguments", Target::None, Expect::Exit0),
    c(11, "forged, stale and stolen handles", Target::StolenHandle, Expect::Exit0),
    c(12, "memory quota exhaustion", Target::None, Expect::Exit0),
    c(13, "endless loop (CPU budget)", Target::None, Expect::Budget),
    c(14, "jump into kernel code", Target::KernelText, Expect::Fault(INSN_FAULT, At::Jump)),
    c(15, "write mtvec (hijack traps)", Target::None, Expect::Fault(ILLEGAL, At::InText)),
    // ESP32-P4 only (#613): core-local extension state that _start leaves OFF, and the ICache after a reload. Not on
    // qemu-virt, whose gate pins its 21 cases.
    #[cfg(feature = "esp32p4")]
    c(16, "FPU instruction (FS off)", Target::None, Expect::Fault(ILLEGAL, At::InText)),
    #[cfg(feature = "esp32p4")]
    c(17, "hardware loop setup (HWLP off)", Target::None, Expect::Fault(ILLEGAL, At::InText)),
    #[cfg(feature = "esp32p4")]
    c(18, "PIE vector load (PIE off)", Target::None, Expect::Fault(ILLEGAL, At::InText)),
    #[cfg(feature = "esp32p4")]
    c(19, "other image in the same slot runs", Target::None, Expect::Reload),
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
    /// The running case's target (a1), for judging where its fault happened.
    target: usize,
    pass: u32,
    fail: u32,
}
impl Harness {
    pub const fn new() -> Self {
        Self { next: 0, running: None, judging: None, target: 0, pass: 0, fail: 0 }
    }
}

fn kcanary() -> u32 {
    // SAFETY: plain read of a kernel static; apps cannot hold references to it.
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(crate::KCANARY)) }
}

#[cfg(feature = "qemu-virt")]
pub fn boot(k: &mut Kernel) -> ! {
    load_b(k);
    enter(k)
}

/// Load app B into slot 0. Returns its image, for the caller's own checks of what landed in the slot.
pub fn load_b(k: &mut Kernel) -> &'static [u8] {
    let s = platform::slot(B);
    if let Err(e) = loader::load(k, B, "B", HEARTBEAT, s, [0; 4], None) {
        kprintln!("boot: heartbeat image refused: {e:?}");
        platform::exit(6);
    }
    kprintln!(
        "boot: B loaded ({} bytes) at text {:#x} data {:#x}, A image {} bytes",
        HEARTBEAT.len(),
        s.text,
        s.data,
        ATTACKER.len()
    );
    HEARTBEAT
}

/// Install the process trap vector, start the tick and enter B. From here on the kernel runs only inside traps: the
/// harness never calls `task_leave`, it ends the run from trap context (`finish`).
pub fn enter(k: &mut Kernel) -> ! {
    csr::clear_mscratch();
    csr::set_mtvec(trap::task_trap_entry as *const () as usize | platform::MTVEC_MODE);
    platform::timer_init();
    k.current = B;
    let ctx = crate::sched::schedule_current(k);
    // Review-617 hardening: once, before the first entry to U, the hardware must hold what the shadow says.
    if !crate::pmp::shadow_matches() {
        kprintln!("harness: the PMP does not match its shadow: halting");
        platform::exit(4);
    }
    csr::mstatus_return_to_user();
    trap::check_return(k, ctx);
    // SAFETY: B's context was set up by the loader; mstatus.MPP = U, the PMP holds B's view, mtvec points at
    // task_trap_entry, the tick is armed, and task_restore loads mscratch. Control comes back only through
    // task_trap_entry; `k`'s borrow is not used past this call.
    unsafe { trap::task_run(ctx) };
    kprintln!("harness: task_leave on core 0 (never called there)");
    platform::exit(5)
}

fn record(k: &mut Kernel, case: &Case, ok: bool, detail: core::fmt::Arguments) {
    if ok {
        k.harness.pass += 1;
    } else {
        k.harness.fail += 1;
    }
    #[cfg(feature = "qemu-virt")]
    kprintln!("TEST {:3} {:34} {} ({})", case.id, case.name, if ok { "PASS" } else { "FAIL" }, detail);
    #[cfg(feature = "esp32p4")]
    crate::bringup_iso::row(case.id, case.name, ok, detail);
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

const RELOAD_LEN: usize = 32;

/// A 32-byte image for A's slot that is not the attacker: header, then `li a0, 0x613; li a7, EXIT; ecall`. Its entry
/// is the attacker's entry address (text + 20) with different bytes, so the run shows the ICache dropped the
/// attacker's code: stale lines would run the attacker's _start instead, which exits with another code.
fn reload_image<'a>(s: &crate::sched::Slot, buf: &'a mut [u8; RELOAD_LEN]) -> &'a [u8] {
    const LI_A0: u32 = ((RELOAD_EXIT as u32) << 20) | (10 << 7) | 0x13; // addi a0, zero, 0x613
    const LI_A7: u32 = ((lichen_abi::sys::EXIT as u32) << 20) | (17 << 7) | 0x13; // addi a7, zero, EXIT
    const ECALL: u32 = 0x73;
    let words = [
        lichen_abi::IMAGE_MAGIC,
        lichen_abi::ABI_VERSION,
        (s.text + 20) as u32,
        s.data as u32,
        (s.text + RELOAD_LEN) as u32,
        LI_A0,
        LI_A7,
        ECALL,
    ];
    for (i, w) in words.iter().enumerate() {
        buf[4 * i..4 * i + 4].copy_from_slice(&w.to_le_bytes());
    }
    &buf[..]
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
            // TooBig: a slot shorter than the image, through the same load path as the others.
            let s = if kind == LoadError2::TooBig { crate::sched::Slot { len: 16, ..slot } } else { slot };
            let r = loader::load(k, A, "A", &buf[..n], s, [0; 4], Some(BUDGET));
            let want = match kind {
                LoadError2::Magic => LoadError::Magic,
                LoadError2::Abi => LoadError::Abi,
                LoadError2::Entry => LoadError::Entry,
                LoadError2::Heap => LoadError::Heap,
                LoadError2::TooBig => LoadError::TooBig,
            };
            let ok = r.as_ref().err() == Some(&want) && k.procs[A].state != State::Ready;
            record(k, case, ok, format_args!("loader said {r:?}"));
            continue;
        }
        let bdata = k.procs[B].slot.data;
        let reload = matches!(case.expect, Expect::Reload);
        let mut small = [0u8; RELOAD_LEN];
        let image: &[u8] = if reload { reload_image(&slot, &mut small) } else { ATTACKER };
        // Entry and heap start of A's own image, for the OwnText / OwnData targets (a bad image is refused below).
        let (entry, heap) = loader::validate(image, &slot).unwrap_or((0, 0));
        let a1 = match case.target {
            Target::None => 0,
            Target::KernelData => core::ptr::addr_of!(crate::KCANARY) as usize,
            Target::KernelText => crate::platform::init as *const () as usize,
            Target::OtherApp => bdata + 64,
            Target::Mmio => platform::MMIO_TARGET,
            Target::StolenHandle => k.handles.first_of(B).unwrap_or(0),
            Target::OwnData => heap,
            Target::OwnText => entry,
        };
        k.harness.target = a1;
        let args = [case.id, a1, bdata + 64, core::ptr::addr_of!(crate::KCANARY) as usize];
        #[cfg(feature = "esp32p4")]
        crate::bringup_iso::begin_case(case.id);
        match loader::load(k, A, "A", image, slot, args, Some(BUDGET)) {
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
    if k.procs[B].state != State::Ready {
        kprintln!("B is no longer running: {:?}", k.procs[B].end);
        finish(k);
    }
    if k.harness.running.is_none() && k.harness.next == 0 {
        start_next(k);
        return;
    }
    if let Some(ci) = k.harness.running {
        if k.procs[A].state == State::Ended {
            k.harness.judging = Some((ci, k.procs[A].end.unwrap(), k.procs[B].syscalls));
            k.harness.running = None;
            k.procs[A].state = State::Free;
        }
        return;
    }
    // B must make at least one more system call after A ended: proof it survived, not just that it is marked Ready.
    let Some((ci, end, b_then)) = k.harness.judging else { return };
    if k.procs[B].syscalls <= b_then {
        return;
    }
    k.harness.judging = None;
    let case = &CASES[ci];
    let text = k.procs[A].slot;
    let in_text = |pc: usize| pc >= text.text && pc < text.text + text.len;
    let target = k.harness.target;
    let as_expected = match (case.expect, end) {
        (Expect::Fault(c, at), End::Fault { cause, tval, pc }) => {
            c == cause
                && match at {
                    At::Target => tval == target && in_text(pc),
                    At::Jump => tval == target && pc == target,
                    At::InText => in_text(pc),
                }
        }
        (Expect::Exit0, End::Exit(0)) => true,
        (Expect::Budget, End::Budget) => true,
        (Expect::Reload, End::Exit(code)) => code == RELOAD_EXIT,
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
    let b = k.procs[B].state == State::Ready;
    kprintln!(
        "SUMMARY: {} pass, {} fail, B {} after {} syscalls, {} ticks",
        k.harness.pass,
        k.harness.fail,
        if b { "still running" } else { "DEAD" },
        k.procs[B].syscalls,
        k.ticks
    );
    #[cfg(feature = "esp32p4")]
    crate::bringup_iso::done(k.harness.pass, k.harness.fail, CASES.len() as u32, b);
    #[cfg(feature = "qemu-virt")]
    platform::exit(if k.harness.fail == 0 && b { 0 } else { 1 })
}
