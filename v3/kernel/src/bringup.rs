//! The ESP32-P4 bring-up run (#611): banner with the build id, a CHECK line per fact the bench wants, END.
//! There is no trap handler until #612 (only the fatal reporter below), so every step that touches hardware is
//! announced with `BEGIN <id>` and drained first: a hang then names the step.
use crate::platform::{self, hw};
use crate::{kcheck, kprintln};
use lichen_kcore::check::{Tally, Verdict};
use lichen_kcore::l2;

pub const BUILD_ID: &str = env!("LICHEN_BUILD_ID");

extern "C" {
    static _load_start: u8;
    static _load_end: u8;
    static _noload_start: u8;
    static _noload_end: u8;
}

fn addr(s: &u8) -> usize {
    s as *const u8 as usize
}

fn hartid() -> usize {
    let v: usize;
    // SAFETY: mhartid is a read-only CSR with no side effects.
    unsafe { core::arch::asm!("csrr {0}, mhartid", out(reg) v, options(nomem, nostack)) };
    v
}

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

pub fn run(hart: usize, boot_info: *const u8) -> ! {
    platform::init();
    hw::uart::spin(2_000_000); // esptool is still reading the MEM_END reply
    kprintln!("");
    kprintln!(
        "lichen-v3 {BUILD_ID} platform {} abi {} (#611 bring-up, RAM only)",
        platform::NAME,
        lichen_abi::ABI_VERSION
    );
    let mut t = Tally::new();

    record(&mut t, "boot.hart", Some(hartid() == 0 && hart == 0), format_args!("mhartid={} a0={hart}", hartid()));
    record(
        &mut t,
        "boot.info",
        None,
        format_args!("a1={boot_info:p} ({})", if boot_info.is_null() { "load-ram" } else { "stage-booted" }),
    );

    // SAFETY: linker-defined symbols; only their addresses are used.
    let (ls, le, ns, ne) = unsafe { (addr(&_load_start), addr(&_load_end), addr(&_noload_start), addr(&_noload_end)) };
    let fits = ls >= hw::mem::LOAD_BASE
        && le <= hw::mem::LOAD_LIMIT
        && ns >= hw::mem::NOLOAD_BASE
        && ne <= hw::mem::NOLOAD_LIMIT;
    record(
        &mut t,
        "image.layout",
        Some(fits),
        format_args!("loaded {ls:#x}..{le:#x} ({} B), noload {ns:#x}..{ne:#x} ({} B)", le - ls, ne - ns),
    );

    begin("c6");
    hw::c6::release();
    record(
        &mut t,
        "c6.strap",
        Some(hw::c6::strap_level()),
        format_args!("gpio13 level={}", hw::c6::strap_level() as u8),
    );
    record(&mut t, "c6.en", Some(hw::c6::en_level()), format_args!("gpio12 level={}", hw::c6::en_level() as u8));

    begin("l2");
    let reg = hw::cache::l2_cachesize_reg();
    match l2::cache_size_bytes(reg) {
        Some(size) => {
            record(
                &mut t,
                "l2.cache",
                None,
                format_args!("size={} KB reg={reg:#x} carve={:#x}", size / 1024, l2::carve_start(size)),
            );
            record(
                &mut t,
                "l2.fit",
                Some(ne <= l2::carve_start(size)),
                format_args!("noload end {ne:#x} vs carve {:#x}", l2::carve_start(size)),
            );
        }
        None => record(&mut t, "l2.cache", Some(false), format_args!("unreadable size reg={reg:#x}")),
    }

    let dropped = platform::take_dropped();
    record(&mut t, "log.ring", Some(dropped == 0), format_args!("dropped={dropped}"));

    let _ = lichen_kcore::check::write_end(&mut crate::console::Console, t.end_reason());
    platform::park()
}

/// Entered from the trap vector (boot_esp32p4.S) on its own stack. No recovery before #612: report and stop.
#[no_mangle]
extern "C" fn fatal_trap() -> ! {
    let (cause, epc, tval): (usize, usize, usize);
    // SAFETY: reading trap CSRs has no side effects.
    unsafe {
        core::arch::asm!("csrr {0}, mcause", "csrr {1}, mepc", "csrr {2}, mtval",
            out(reg) cause, out(reg) epc, out(reg) tval, options(nomem, nostack));
    }
    kprintln!("FATAL trap mcause={cause:#x} mepc={epc:#x} mtval={tval:#x}");
    platform::exit(3)
}
