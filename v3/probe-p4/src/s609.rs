//! #609: the five hardware checks left before a v3 kernel goes on the badge, run behind the stock v6.1 bootloader as
//! an app image in ota_1 (LICHEN_PROBE_RUN=app, #608 step 3b). LICHEN_PROBE_SUITE picks one check per image:
//! - c1pmp (check 1): core 0 starts core 1 (core1.rs, IDF's order); core 1 dumps its PMP and PMA (B5).
//! - pmp1 (check 2): U reads and writes the CPU-peripheral window that the bootloader's locked pmp1 grants every mode.
//! - c1timer (check 3): core 1's CLINT timer preempts a U spin loop TICKS times; period and jitter in mtime ticks.
//! - dgpio (check 5): U reads and writes of the dedicated-GPIO CSRs 0x803-0x805.
//!
//! Each suite ends in one verdict, `CHECK [C1.]609.<n> PASS|FAIL <probe id> ...` (C1. when core 1 prints it). PASS
//! means the isolation property holds (check 1: the kernel owns all 16 core-1 PMP entries); FAIL is either a hole or a
//! void run, and says which. The run ends in the ROM reset, so the bootloader finds ota_1 still pending-verify and
//! boots ota_0. The bootloader's 9 s RTC watchdog, never fed, is the backstop for a bus stall that no trap reports.

use crate::hw::{self, m_lbu, m_lw, m_sb, m_sw, napot, pmp_cfg_byte, pmp_off, pmp_set, pmpaddr_r, user, Trap};
use crate::hw::{NAPOT, R, W, X};
use crate::{app_desc, b6x, begin, check, clic_byte, core1, show, suite_is, uaddr, unlocked, ShowB, ShowU};
use crate::{CLIC_THRESH, E_UDATA, E_UTEXT, GPIO_BASE, MTIMECMP_HI, MTIMECMP_LO, MTIMECTL, MTIME_LO, MTIP_ID};
use crate::{PROBE_ID, SUITE, UDATA, UTEXT};
use core::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};

const SUITES: [&str; 4] = ["c1pmp", "pmp1", "c1timer", "dgpio"];
pub const fn known(s: &str) -> bool {
    let mut i = 0;
    while i < SUITES.len() {
        if crate::str_eq(s, SUITES[i]) {
            return true;
        }
        i += 1;
    }
    false
}
const fn verdict_id() -> &'static str {
    if suite_is("c1pmp") {
        "609.1"
    } else if suite_is("pmp1") {
        "609.2"
    } else if suite_is("c1timer") {
        "609.3"
    } else if suite_is("psram") {
        "609.4"
    } else {
        "609.5"
    }
}
const ID: &str = verdict_id();
fn verdict(ok: bool, detail: core::fmt::Arguments) {
    check(ID, Some(ok), format_args!("{PROBE_ID} {detail}"));
}

/// Core 0 entry from pmain. Never returns: the ROM reset ends every suite, as in step 3b.
pub fn run() -> ! {
    pl!("lichen-probe-p4 {PROBE_ID} (#609 {SUITE}, check {ID}), core 0, v6.1 bootloader state: app image in ota_1");
    if const { suite_is("c1pmp") || suite_is("c1timer") } {
        on_core1();
    }
    if const { suite_is("pmp1") } {
        pmp1();
    }
    if const { suite_is("dgpio") } {
        dgpio();
    }
    pl!("END ok");
    hw::drain();
    app_desc::reset()
}

/// The kernel's starting point on this hart: every PMP entry the bootloader left unlocked is cleared, so U holds only
/// its text and stack pages plus whatever the locked entries grant. Unlike setup_pmp this prints no SETUP FAIL for
/// the bootloader's locked entry 15, which no #609 suite needs.
fn setup_u() {
    let mut locked = 0u32;
    for i in 0..16 {
        if unlocked(i) {
            pmp_off(i);
        } else {
            locked |= 1 << i;
        }
    }
    pmp_set(E_UTEXT, napot(UTEXT, 0x1000), NAPOT | R | X);
    pmp_set(E_UDATA, napot(UDATA, 0x1000), NAPOT | R | W);
    pl!("609 setup: unlocked PMP entries cleared (locked mask {locked:#06x}); U text {UTEXT:#x} RX entry {E_UTEXT}, U stack {UDATA:#x} RW entry {E_UDATA}");
}

// Checks 1 and 3, core 0's side -------------------------------------------------------------------------------------
/// Start core 1, wait for it (2 s of silence ends the wait), then stall it and clear its boot address before the reset.
fn on_core1() {
    begin("609.core0");
    let a = crate::mtime();
    hw::spin(100_000);
    let b = crate::mtime();
    let (Ok(a), Ok(b)) = (a, b) else {
        verdict(false, format_args!("void: core 0 cannot read its mtime ({} {}), so it cannot time core 1", ShowM(a), ShowM(b)));
        return;
    };
    if b <= a {
        verdict(false, format_args!("void: core 0 mtime does not count ({a:#x} -> {b:#x})"));
        return;
    }
    pl!("609 core 0 mtime {a:#x} -> {b:#x} over 100k spin loops; core 1 is given up after 2 s with no BEGIN");
    core1::start();
    let silent = core1::watch_mtime(2000);
    core1::stop();
    if let Some(s) = silent {
        verdict(false, format_args!("void: core 1 silent at stage {s} (the last C1 BEGIN above); core 0 stalled it"));
    }
}
struct ShowM(Result<u64, Trap>);
impl core::fmt::Display for ShowM {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        match self.0 {
            Ok(v) => write!(f, "{v:#x}"),
            Err(t) => write!(f, "TRAP({t})"),
        }
    }
}

/// Core 1 entry from p1main (core1.rs), which reports done afterwards.
pub fn core1_suite() {
    if const { suite_is("c1pmp") } {
        c1pmp();
    }
    if const { suite_is("c1timer") } {
        c1timer();
    }
}

// Check 1 -----------------------------------------------------------------------------------------------------------
const PMP_A: u8 = 0x18;
const PMA_L: usize = 1 << 29; // riscv/csr.h PMA_L; the bootloader sets it on all 16 of core 0's PMA entries (3b)
fn pma_cfg(i: usize) -> Result<usize, Trap> {
    macro_rules! arms {
        ($($i:literal => $c:literal),*) => { match i { $($i => csr_try!($c),)* _ => Ok(0) } };
    }
    arms!(0 => "0xbc0", 1 => "0xbc1", 2 => "0xbc2", 3 => "0xbc3", 4 => "0xbc4", 5 => "0xbc5", 6 => "0xbc6",
          7 => "0xbc7", 8 => "0xbc8", 9 => "0xbc9", 10 => "0xbca", 11 => "0xbcb", 12 => "0xbcc", 13 => "0xbcd",
          14 => "0xbce", 15 => "0xbcf")
}
fn c1pmp() {
    crate::b5_dump(); // B5, B5.clic and B5.upper on core 1: the guarded CSR reads of run 8 and step 3b
    let locked = (0..16).filter(|&i| !unlocked(i)).count();
    let used = (0..16).filter(|&i| pmp_cfg_byte(i) & PMP_A != 0).count();
    let addrs = (0..16).filter(|&i| pmpaddr_r(i) != 0).count();
    let (mut pma_read, mut pma_locked) = (0, 0);
    for i in 0..16 {
        if let Ok(c) = pma_cfg(i) {
            pma_read += 1;
            pma_locked += (c & PMA_L != 0) as u32;
        }
    }
    verdict(locked == 0, format_args!("core 1 behind the v6.1 bootloader: {locked} of 16 PMP entries locked, {used} in use (A != OFF), {addrs} with a nonzero address; PMA: {pma_locked} of {pma_read} readable entries locked (bit 29); PASS = the kernel owns all 16 core-1 PMP entries"));
}

// Check 3 -----------------------------------------------------------------------------------------------------------
const TICKS: usize = 50;
const PERIOD: u64 = 40_000; // 1 ms of the 40 MHz mtime (#608 run 9)
const JITTER_MAX: u32 = (PERIOD / 100) as u32; // 10 us
// Written by tick() in core 1's trap handler and read after the U run: one hart, never concurrently.
static mut T_AT: [u32; TICKS] = [0; TICKS];
static mut T_CMP: [u32; TICKS] = [0; TICKS];
static mut T_ODD: u32 = 0;
static mut CMP: u64 = 0;

fn set_cmp_raw(c: u64) {
    // SAFETY: own-core CLINT mtimecmp, checked readable from M before the run; the high word is parked first so the
    // 64-bit compare never matches a half-written value.
    unsafe {
        write_volatile(MTIMECMP_HI as *mut u32, u32::MAX);
        write_volatile(MTIMECMP_LO as *mut u32, c as u32);
        write_volatile(MTIMECMP_HI as *mut u32, (c >> 32) as u32);
    }
}
/// The IRQ_QUIESCE hook: stamp the entry first, then re-arm one period after the last compare (not after now, so the
/// period cannot drift), or silence the timer after the last tick. Raw accesses: a fault here would nest the trap.
fn tick() {
    // SAFETY: core 1's trap handler, M interrupts off; the statics belong to this suite (see above).
    unsafe {
        let now = read_volatile(MTIME_LO as *const u32);
        let n = read_volatile(addr_of!(hw::IRQ_COUNT)) as usize;
        let last = read_volatile(addr_of!(hw::IRQ_LAST));
        let cmp = read_volatile(addr_of!(CMP));
        let from_u = (last.mstatus >> 11) & 3 == 0 && last.epc & !0xFFF == UTEXT && last.code() == MTIP_ID;
        if (1..=TICKS).contains(&n) {
            (*addr_of_mut!(T_AT))[n - 1] = now;
            (*addr_of_mut!(T_CMP))[n - 1] = cmp as u32;
        }
        if !from_u || n > TICKS {
            write_volatile(addr_of_mut!(T_ODD), read_volatile(addr_of!(T_ODD)) + 1);
        }
        if n < TICKS {
            write_volatile(addr_of_mut!(CMP), cmp + PERIOD);
            set_cmp_raw(cmp + PERIOD);
        } else {
            write_volatile(clic_byte(0, MTIP_ID, 1) as *mut u8, 0);
            set_cmp_raw(u64::MAX);
        }
    }
}
fn c1timer() {
    begin("609.3");
    setup_u();
    let saved: [Result<u8, Trap>; 4] = core::array::from_fn(|b| m_lbu(clic_byte(0, MTIP_ID, b)));
    let thresh = m_lw(CLIC_THRESH);
    pl!("609.3 CLINT mtimectl={} cmp lo/hi={} {}; CLIC id 7 ip/ie/attr/ctl {} {} {} {}, thresh {}", show(m_lw(MTIMECTL)),
        show(m_lw(MTIMECMP_LO)), show(m_lw(MTIMECMP_HI)), ShowB(saved[0]), ShowB(saved[1]), ShowB(saved[2]), ShowB(saved[3]), show(thresh));
    let (Ok(_), Ok(_)) = (crate::mtime(), m_lw(MTIMECMP_LO)) else {
        verdict(false, format_args!("void: core 1 cannot read its CLINT from M"));
        return;
    };
    crate::quiesce_timer();
    // Calibrate the U loop with nothing armed, so the run below outlasts TICKS periods about twice.
    let a = core1::mtime_raw();
    let t = user(uaddr(crate::u_spin), 100_000, 0, false);
    let per = core1::mtime_raw().wrapping_sub(a);
    if !t.is_ecall_u() || per == 0 {
        verdict(false, format_args!("void: calibration U spin ended {t} after {per} ticks"));
        return;
    }
    let loops = ((TICKS as u64 + 4) * PERIOD * 2 * 100_000 / per).min(0xFFFF_FFF0);
    pl!("609.3 calibration: {per} mtime ticks per 100k U spin loops; U spins {loops} loops (~{} ms)", loops * per / 100_000 / 40_000);
    // SAFETY: core 1 only; the handler reads the hook only on an interrupt.
    unsafe {
        write_volatile(addr_of_mut!(hw::IRQ_QUIESCE), tick);
        write_volatile(addr_of_mut!(T_ODD), 0);
    }
    let _ = m_sb(clic_byte(0, MTIP_ID, 2), 0xC0); // mode M, level (the CLINT line is level-type), as B3t
    let _ = m_sb(clic_byte(0, MTIP_ID, 3), 0xFF);
    let _ = m_sw(CLIC_THRESH, 0);
    let _ = m_lw(CLIC_THRESH); // the threshold takes effect after a read-back (interrupt_clic.h)
    crate::set_irq_count(0);
    let s0 = b6x::systimer();
    let m0 = core1::mtime_raw();
    let first = m0 + 4 * PERIOD;
    // SAFETY: as above; the timer is not armed yet.
    unsafe { write_volatile(addr_of_mut!(CMP), first) };
    set_cmp_raw(first);
    let _ = m_sb(clic_byte(0, MTIP_ID, 1), 1);
    let t = user(uaddr(crate::u_spin), loops as usize, 0, false);
    let m1 = core1::mtime_raw();
    let s1 = b6x::systimer();
    crate::quiesce_timer();
    // SAFETY: the timer is silenced; back to the default hook.
    unsafe { write_volatile(addr_of_mut!(hw::IRQ_QUIESCE), || {}) };
    for b in [2, 3] {
        if let Ok(v) = saved[b] {
            let _ = m_sb(clic_byte(0, MTIP_ID, b), v);
        }
    }
    if let Ok(v) = thresh {
        let _ = m_sw(CLIC_THRESH, v);
    }
    let ratio = match (s0, s1) {
        (Some(s0), Some(s1)) if s1 > s0 => (m1 - m0) * 1000 / (s1 - s0),
        _ => 0,
    };
    timer_report(t, ratio);
}
fn timer_report(t: Trap, ratio: u64) {
    let n = crate::irq_count() as usize;
    // SAFETY: the timer is silenced, the handler no longer writes these.
    let (at, cmp, odd) = unsafe { (*addr_of!(T_AT), *addr_of!(T_CMP), read_volatile(addr_of!(T_ODD))) };
    let got = n.min(TICKS);
    let (mut dmin, mut dmax, mut dsum) = (u32::MAX, 0u32, 0u64);
    for k in 1..got {
        let d = at[k].wrapping_sub(at[k - 1]);
        (dmin, dmax, dsum) = (dmin.min(d), dmax.max(d), dsum + d as u64);
    }
    let (mut lmin, mut lmax) = (u32::MAX, 0u32);
    for k in 0..got {
        let l = at[k].wrapping_sub(cmp[k]);
        (lmin, lmax) = (lmin.min(l), lmax.max(l));
    }
    pl!("609.3 first intervals {} {} {} {} {} {} {} {}; first entry latencies {} {} {} {}", at[1].wrapping_sub(at[0]),
        at[2].wrapping_sub(at[1]), at[3].wrapping_sub(at[2]), at[4].wrapping_sub(at[3]), at[5].wrapping_sub(at[4]),
        at[6].wrapping_sub(at[5]), at[7].wrapping_sub(at[6]), at[8].wrapping_sub(at[7]), at[0].wrapping_sub(cmp[0]),
        at[1].wrapping_sub(cmp[1]), at[2].wrapping_sub(cmp[2]), at[3].wrapping_sub(cmp[3]));
    let mean = if got > 1 { dsum / (got as u64 - 1) } else { 0 };
    let jitter = if got > 1 { dmax - dmin } else { u32::MAX };
    let ok = n == TICKS && odd == 0 && t.is_ecall_u() && t.a0 == 0 && jitter <= JITTER_MAX;
    verdict(ok, format_args!("core 1 CLINT timer, period {PERIOD} ticks (1 ms at 40 MHz): {n} of {TICKS} interrupts, {odd} not MTIP from U text; interval min/mean/max {dmin}/{mean}/{dmax} ticks, jitter {jitter} (PASS <= {JITTER_MAX}); entry latency {lmin}-{lmax} ticks; U ended {t} a0={:#x}; mtime/systimer x1000 = {ratio} (2500 = 40 MHz, 0 = systimer not counting)", t.a0));
}

// Check 2 -----------------------------------------------------------------------------------------------------------
const SOC_CLK_CTRL0: usize = 0x500E_6014; // hp_sys_clkrst_reg.h: bit 6 BUSMON_CPU_CLK_EN, bit 11 TRACE_CPU_CLK_EN
const BUSMON_TRACE_CLK: u32 = (1 << 6) | (1 << 11);
const BUSMON_INTR_ENA: usize = 0x3FF0_6000; // ASSIST_DEBUG_CORE_0_INTR_ENA_REG
const PMP1_CFG: u8 = 0x9B; // the bootloader's entry 1 (step 3b): L, NAPOT, RW
const PMP1_ADDR: usize = 0x0FFC_3FFF; // NAPOT 0x3ff0_0000-0x3ff1_ffff
/// Core-0 instances only (TRACE0, CORE_0 bus monitor): core 1 is in reset, and its windows may stall the bus (run 1).
/// The written ones are plain R/W with no trigger or self-clearing bit, and inert while their unit is idle.
struct Reg {
    id: &'static str,
    addr: usize,
    write: bool,
}
const PMP1_REGS: [Reg; 6] = [
    Reg { id: "609.2.cache-date", addr: 0x3FF1_03FC, write: false }, // CACHE_DATE_REG
    Reg { id: "609.2.cache-sync-addr", addr: 0x3FF1_00A0, write: true }, // CACHE_SYNC_ADDR_REG; ops start from SYNC_CTRL
    Reg { id: "609.2.busmon-date", addr: 0x3FF0_63FC, write: false }, // ASSIST_DEBUG_DATE_REG
    Reg { id: "609.2.busmon-area-min", addr: 0x3FF0_6010, write: true }, // ASSIST_DEBUG_CORE_0_AREA_DRAM0_0_MIN_REG
    Reg { id: "609.2.trace0-date", addr: 0x3FF0_43FC, write: false }, // TRACE_DATE_REG(0)
    Reg { id: "609.2.trace0-mem-start", addr: 0x3FF0_4000, write: true }, // TRACE_MEM_START_ADDR_REG(0)
];
/// What a U access achieved. Denied: it faulted. Dropped: no fault and no effect, while M's own write sticks.
#[derive(Clone, Copy, PartialEq)]
enum Seen {
    Denied,
    Dropped,
    Reached,
    Void,
}
impl core::fmt::Display for Seen {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        f.write_str(match self {
            Seen::Denied => "DENIED (faulted)",
            Seen::Dropped => "DROPPED (no fault, no effect)",
            Seen::Reached => "REACHED",
            Seen::Void => "VOID",
        })
    }
}
fn pmp1() {
    begin("609.2");
    let (cfg, addr) = (pmp_cfg_byte(1), pmpaddr_r(1));
    if cfg != PMP1_CFG || addr != PMP1_ADDR {
        verdict(false, format_args!("void: pmp1 is cfg {cfg:#04x} addr {addr:#010x}, not the bootloader's {PMP1_CFG:#04x} {PMP1_ADDR:#010x}"));
        return;
    }
    setup_u();
    let Ok(clk) = m_lw(SOC_CLK_CTRL0) else {
        verdict(false, format_args!("void: soc_clk_ctrl0 not readable"));
        return;
    };
    // Both clocks are off in ROM state (run 8); the test runs with them on, the case a kernel would face.
    let _ = m_sw(SOC_CLK_CTRL0, clk | BUSMON_TRACE_CLK);
    pl!("609.2 pmp1 cfg {cfg:#04x} addr {addr:#010x}; soc_clk_ctrl0 {clk:#010x} -> {} (bus monitor and trace clocks on)", show(m_lw(SOC_CLK_CTRL0)));
    let (mut bad, mut void) = (0, 0);
    for r in &PMP1_REGS {
        begin(r.id);
        // A bus-monitor bound only matters with a monitor armed: then U writes the value already there.
        let same = r.addr == 0x3FF0_6010 && m_lw(BUSMON_INTR_ENA) != Ok(0);
        for s in pmp1_reg(r, same) {
            bad += (s == Seen::Reached) as u32;
            void += (s == Seen::Void) as u32;
        }
    }
    let _ = m_sw(SOC_CLK_CTRL0, clk);
    verdict(bad == 0 && void == 0, format_args!("U through the locked pmp1 (cache, bus monitor, trace0): {bad} access(es) reached a register, {void} void; PASS = the window is privilege-checked for U (every U access faults or is dropped)"));
}
/// One register: U read, then (if r.write) an M write control and the U write; M restores the value after each write.
fn pmp1_reg(r: &Reg, same: bool) -> [Seen; 2] {
    let old = match m_lw(r.addr) {
        Ok(v) => v,
        Err(t) => {
            check(r.id, None, format_args!("{:#x}: M read traps {t}: skipped", r.addr));
            return [Seen::Void, if r.write { Seen::Void } else { Seen::Denied }];
        }
    };
    let t = user(uaddr(crate::u_load), r.addr, 0, false);
    let read = if t.is_ecall_u() {
        Seen::Reached
    } else if t.code() == 5 && t.tval == r.addr {
        Seen::Denied
    } else {
        Seen::Void
    };
    check(r.id, None, format_args!("{:#x}: M reads {old:#x}; U read {t} a0={:#x}: {read}", r.addr, t.a0));
    if !r.write {
        return [read, Seen::Denied];
    }
    let test = if same { old } else { old ^ 0x0001_0000 };
    let _ = m_sw(r.addr, test);
    let m_back = m_lw(r.addr);
    let _ = m_sw(r.addr, old);
    let sticks = m_back == Ok(test) && test != old;
    let t = user(uaddr(crate::u_store), r.addr, test as usize, false);
    let after = m_lw(r.addr);
    let _ = m_sw(r.addr, old);
    let restored = m_lw(r.addr);
    let write = if !t.is_ecall_u() {
        if t.code() == 7 && t.tval == r.addr { Seen::Denied } else { Seen::Void }
    } else if after == Ok(test) && test != old {
        Seen::Reached
    } else if after == Ok(old) && sticks {
        Seen::Dropped
    } else {
        Seen::Void // the register ignores M too (or U wrote the same value): the U store's effect is not observable
    };
    check(r.id, None, format_args!("{:#x}: M writes {test:#x}, reads {} (sticks: {sticks}); U writes it: {t}; M then reads {}: {write}; restored {}",
        r.addr, show(m_back), show(after), show(restored)));
    [read, write]
}

// Check 5 -----------------------------------------------------------------------------------------------------------
const GPIO_OUT_SEL0: usize = GPIO_BASE + 0x558; // GPIO_FUNC0_OUT_SEL_CFG_REG, one per pad, [8:0] signal
const GPIO_IN_SEL214: usize = GPIO_BASE + 0x4B0; // GPIO_FUNC214_IN_SEL_CFG_REG: [5:0] pad, bit 7 through the matrix
const PADS: usize = 55; // SOC_GPIO_PIN_COUNT
const CORE_GPIO: core::ops::RangeInclusive<u32> = 214..=241; // CORE_GPIO_{IN,OUT}_PAD_{IN,OUT}0-27 (gpio_sig_map.h)

extern "C" {
    fn u_csrw803();
    fn u_csrw804();
    fn u_csrw805();
}
macro_rules! csr_write_try {
    ($n:literal, $v:expr) => {{
        let v: usize = $v;
        hw::guarded(|| {
            // SAFETY: guarded: a CSR that refuses the write traps and is skipped (4-byte encoding forced).
            unsafe {
                core::arch::asm!(".option push", ".option norvc", concat!("csrw ", $n, ", {0}"), ".option pop",
                    in(reg) v, options(nostack))
            };
            0
        })
    }};
}
/// Read-only scan: which pads the GPIO matrix routes to the core's dedicated-GPIO signals.
fn dgpio_routes() -> (u32, u32) {
    let (mut out, mut inp, mut err) = (0, 0, 0);
    for pad in 0..PADS {
        match m_lw(GPIO_OUT_SEL0 + 4 * pad) {
            Ok(v) if CORE_GPIO.contains(&(v & 0x1FF)) => {
                out += 1;
                pl!("609.5 pad {pad} out_sel {} (CORE_GPIO_OUT{})", v & 0x1FF, (v & 0x1FF) - 214);
            }
            Ok(_) => {}
            Err(_) => err += 1,
        }
    }
    for sig in CORE_GPIO {
        match m_lw(GPIO_IN_SEL214 + 4 * (sig - 214) as usize) {
            Ok(v) if v & 0x80 != 0 && v & 0x3F < PADS as u32 => {
                inp += 1;
                pl!("609.5 CORE_GPIO_IN{} reads pad {}", sig - 214, v & 0x3F);
            }
            Ok(_) => {}
            Err(_) => err += 1,
        }
    }
    pl!("609.5 GPIO matrix: {out} of {PADS} pads driven by a CORE_GPIO output, {inp} CORE_GPIO inputs fed by a pad, {err} registers unreadable");
    (out + err, inp)
}
fn dgpio() {
    begin("609.5");
    setup_u();
    let (routed, inputs) = dgpio_routes();
    // With a pad routed a changed value could drive it: U then writes the value already there, and only a trap counts.
    let flip = if routed == 0 { 1 } else { 0 };
    macro_rules! one {
        ($n:literal, $ur:ident, $uw:ident) => {{
            let id = concat!("609.5.", $n);
            begin(id);
            let m_old = csr_try!($n);
            let u_r = user(uaddr(crate::$ur), 0, 0, false);
            match m_old {
                Err(t) => {
                    check(id, None, format_args!("CSR {} M read traps {t}: void", $n));
                    (u_r.is_ecall_u(), Seen::Void)
                }
                Ok(old) => {
                    let test = old ^ flip;
                    let _ = csr_write_try!($n, test);
                    let m_back = csr_try!($n);
                    let _ = csr_write_try!($n, old);
                    let sticks = m_back == Ok(test) && test != old;
                    let u_w = user(uaddr($uw), 0, test, false);
                    let m_after = csr_try!($n);
                    let _ = csr_write_try!($n, old);
                    let restored = csr_try!($n);
                    let seen = if !u_w.is_ecall_u() {
                        if u_w.code() == 2 { Seen::Denied } else { Seen::Void }
                    } else if m_after == Ok(test) && test != old {
                        Seen::Reached
                    } else if m_after == Ok(old) && (sticks || m_back == Ok(old) && test != old) {
                        Seen::Dropped // ignored for U, or read-only for M as well
                    } else {
                        Seen::Void
                    };
                    check(id, None, format_args!("CSR {}: M reads {old:#x}; U read {u_r} a0={:#x}; M writes {test:#x}, reads {} (sticks: {sticks}); U writes it: {u_w} a0={:#x}; M then reads {}: {seen}; restored {}",
                        $n, u_r.a0, ShowU(m_back), u_w.a0, ShowU(m_after), ShowU(restored)));
                    (u_r.is_ecall_u(), seen)
                }
            }
        }};
    }
    let res = [one!("0x803", u_csr803, u_csrw803), one!("0x804", u_csr804, u_csrw804), one!("0x805", u_csr805, u_csrw805)];
    let reads = res.iter().filter(|r| r.0).count();
    let wrote = res.iter().filter(|r| r.1 == Seen::Reached).count();
    let void = res.iter().filter(|r| r.1 == Seen::Void).count();
    pl!("609.5 kernel rule if U can use them: no pad's GPIO_FUNCn_OUT_SEL selects CORE_GPIO 214-241 and no CORE_GPIO input is routed (FUNC214-241_IN_SEL), so the bundle reaches no pad; now {routed} out, {inputs} in");
    verdict(wrote == 0 && void == 0, format_args!("dedicated-GPIO CSRs 0x803-0x805 from U: {reads} of 3 readable, {wrote} writable, {void} void (pads routed: {routed}, so U wrote {}); PASS = U cannot change them",
        if flip == 1 { "a changed value" } else { "the same value only" }));
}
