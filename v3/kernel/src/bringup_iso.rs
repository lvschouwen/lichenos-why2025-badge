//! The #607 isolation harness on the ESP32-P4 (#613): the last stage of the bring-up run. App B (heartbeat) and app A
//! (attacker), linked for the NOLOAD L2MEM slots of `lichen_kcore::slots`, run in U-mode under the PMP view of their
//! own slot, preempted by the 1 ms CLINT tick. The cases, the scheduler and the judging are the qemu-virt ones
//! (tests.rs, sched.rs); this file only connects them to the bench protocol: a `BEGIN iso.<id>` before each case, one
//! `CHECK iso.<id>` row per case, a summary row, then the bring-up's last rows and END.
//!
//! On top of the 21 shared cases the P4 runs four of its own (tests.rs): FPU, hardware-loop and PIE instructions from
//! U with their state off (illegal), and iso.19, which loads a different image into A's slot after the attacker ran
//! there and checks the new code runs. That case exercises the ICache invalidate in `hw::cache::sync_icache`; the
//! others do not (every attacker load writes the same bytes).
//!
//! The harness takes the hart over for good. It installs its own trap vector (trap.S, full app contexts, so B and A
//! can be switched), and the kernel runs only inside traps from then on. So the bring-up's Tally moves into a static
//! first: the bring-up stack is abandoned on the first entry to U (every trap starts again at `_stack_top`).
use crate::bringup::{self, begin, record};
use crate::global::Global;
use crate::platform::{self, hw};
use crate::{kcheck, sched, tests};
use core::fmt::Write;
use lichen_kcore::check::{Tally, Verdict};

/// The bring-up's Tally while the harness runs. Touched by `run` before the first U entry, then only from trap
/// context (trap_handler, never re-entrant): one `&mut` at a time (Global invariant).
static TALLY: Global<Tally> = Global::new(Tally::new());

/// # Safety
/// As `Global::get`: no other live reference from `tally()`; every caller's borrow ends within its statement.
unsafe fn tally() -> &'static mut Tally {
    TALLY.get()
}

pub fn run(t: Tally) -> ! {
    begin("iso.boot");
    // SAFETY: the first use; no other reference to TALLY exists.
    *unsafe { tally() } = t;
    // The slots must lie below the L2 cache carve the chip really has, or the loader would write into the cache.
    let reg = hw::cache::l2_cachesize_reg();
    let carve = lichen_kcore::l2::cache_size_bytes(reg).map(lichen_kcore::l2::carve_start);
    if carve.is_none_or(|c| lichen_kcore::slots::NOLOAD_LIMIT > c) {
        record(
            // SAFETY: the only reference to TALLY while it lives; `record` does not reach TALLY itself.
            unsafe { tally() },
            "iso.summary",
            Some(false),
            format_args!(
                "harness skipped: app slots end at {:#x}, cache carve {carve:x?}",
                lichen_kcore::slots::NOLOAD_LIMIT
            ),
        );
        // SAFETY: a copy out; the borrow ends at once.
        bringup::finish(*unsafe { tally() })
    }
    // SAFETY: the bring-up holds no reference into core 0's Kernel (only the harness uses it), and no app runs yet.
    let k = unsafe { sched::this() };
    let blob = tests::load_b(k);
    loaded_check(k.procs[0].slot.text, blob);
    begin("iso.enter");
    tests::enter(k)
}

/// The slot text read back through the uncached alias equals the image: the write-back of the loader's stores reached
/// L2MEM itself, so instruction fetch (after the ICache invalidate) sees the app, not stale memory.
fn loaded_check(text: usize, blob: &[u8]) {
    let mut first_bad = None;
    for (i, w) in blob.chunks(4).enumerate() {
        let mut want = [0u8; 4];
        want[..w.len()].copy_from_slice(w);
        // SAFETY: text + 4 * i lies inside slot 0's text region (4-byte aligned, L2MEM), which the loader just wrote.
        let got = unsafe { hw::mailbox::read_uncached(text + 4 * i) };
        if got != u32::from_le_bytes(want) && first_bad.is_none() {
            first_bad = Some(text + 4 * i);
        }
    }
    record(
        // SAFETY: the only reference to TALLY while it lives; `record` does not reach TALLY itself.
        unsafe { tally() },
        "iso.load",
        Some(first_bad.is_none()),
        format_args!(
            "B ({} B) at {text:#x}, uncached readback {}",
            blob.len(),
            match first_bad {
                None => "matches the image",
                Some(_) => "DIFFERS",
            }
        ),
    );
    if let Some(a) = first_bad {
        crate::kprintln!("iso: first differing word at {a:#x}");
    }
}

/// `iso.<case id>`, e.g. `iso.07` or `iso.104`.
fn case_id(id: usize) -> lichen_abi::Line {
    let mut l = lichen_abi::Line::new();
    let _ = write!(l, "iso.{id:02}");
    l
}

/// Before each attacker run: a hang then names the case.
pub fn begin_case(id: usize) {
    begin(case_id(id).as_str());
}

/// One judged case. The ring is 4 KB and the harness prints from trap context only, so every row is drained at once.
pub fn row(id: usize, name: &str, ok: bool, detail: core::fmt::Arguments) {
    let v = if ok { Verdict::Pass } else { Verdict::Fail };
    // SAFETY: trap context, not re-entrant; the borrow ends in this statement.
    unsafe { tally() }.record(v);
    kcheck!(case_id(id).as_str(), v, "{name}: {detail}");
    platform::flush();
}

/// Every case is judged (or B died): stop the tick and end the bring-up run. Called from trap context.
pub fn done(pass: u32, fail: u32, cases: u32, b_alive: bool) -> ! {
    platform::timer_stop();
    record(
        // SAFETY: as in `loaded_check`.
        unsafe { tally() },
        "iso.summary",
        Some(pass == cases && fail == 0 && b_alive),
        format_args!(
            "{pass} of {cases} cases pass, {fail} fail, B {}, mintstatus {:#x}",
            if b_alive { "still running" } else { "DEAD" },
            hw::trap::mintstatus()
        ),
    );
    // SAFETY: a copy out; the borrow ends at once.
    bringup::finish(*unsafe { tally() })
}
