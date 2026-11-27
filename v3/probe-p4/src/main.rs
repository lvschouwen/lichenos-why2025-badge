//! #608 bench probe for the #604 isolation matrix on the ESP32-P4 rev v1.0, in ROM download-mode state, loaded by
//! `esptool load-ram`. Default (run 8): core 0 starts core 1 and watches it; core 1 runs B5, B1, B3 and a U read of the
//! CPU-subsystem window (`core1.rs`). LICHEN_PROBE_CORE0=1 builds the core-0 suite instead: B5, B1, A5, B3, B6, B4
//! and B2 (last: locks hold until reset).
//! Output: one `CHECK <id> <PASS|FAIL|INFO> ...` line per result, `BEGIN <id>` before each risky step, `END` last.
#![no_std]
#![no_main]

#[macro_use]
mod hw;
mod core1;

use core::arch::global_asm;
use core::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};
use hw::{m_lbu, m_lw, m_sb, m_sw, napot, pmp_cfg_byte, pmp_off, pmp_set, user, Trap};
use hw::{L, NAPOT, R, TOR, W, X};

global_asm!(include_str!("asm.S"));

extern "C" {
    fn u_ecall();
    fn u_csr810();
    fn u_csr803();
    fn u_csr804();
    fn u_csr805();
    fn u_load();
    fn u_store();
    fn u_storeb();
    fn u_spin();
    fn u_mret();
    fn u_csrw_pmp();
    static _utext: u8;
    static _sbss: u8;
}

const PROBE_ID: &str = env!("LICHEN_PROBE_ID");
const CORE0_SUITE: bool = option_env!("LICHEN_PROBE_CORE0").is_some();

// PMP entries the probe owns (cleared first, skipped when the ROM left them locked).
const E_UTEXT: usize = 4;
const E_UDATA: usize = 5;
const E_T1: usize = 6;
const E_T2: usize = 7;
const E_TOR_LO: usize = 10;
const E_TOR_HI: usize = 11;
const E_LOCK_NAPOT: usize = 12;
const E_COVER_PERI: usize = 14; // B2: locked M cover of 0x5000_0000-0x5010_0000 (UART0, PMS, GPIO)
const E_COVER_RAM: usize = 15; // B2: locked M cover of 0x4FF0_0000-0x4FF8_0000 (all probe memory)

pub(crate) const UTEXT: usize = 0x4FF1_8000;
const UDATA: usize = 0x4FF5_8000;
const SCRATCH: usize = 0x4FF5_A000; // L2MEM test buffer (4 KB, cached alias)
const SCRATCH_UNCACHED: usize = SCRATCH + 0x4000_0000; // soc.h:223-224 uncached alias
const LOCK_A: usize = 0x4FF5_B000; // B2 NAPOT, 256 B
const LOCK_B: usize = 0x4FF5_B800; // B2 TOR, 256 B

// CLIC (clic_reg.h): per-core window, other core at +0x10000; line i control word at CTRL + 4 * i.
pub(crate) const CLIC_THRESH: usize = 0x2080_0008;
const CLIC_CTRL: usize = 0x2080_1000;
const TEST_CPU_LINE: usize = 25;
const TEST_ID: usize = 16 + TEST_CPU_LINE; // CLIC_EXT_INTR_NUM_OFFSET
pub(crate) const fn clic_byte(core_off: usize, id: usize, byte: usize) -> usize {
    CLIC_CTRL + core_off + 4 * id + byte // 0 ip, 1 ie, 2 attr, 3 ctl
}
// CLINT, own core (TRM v1.0 sec. 2.9.3.5; documented for v3.x, unconfirmed on v1.0 until this probe reads it).
pub(crate) const CLINT: usize = 0x2000_0000;
const MSIP: usize = CLINT;
const MTIMECMP_LO: usize = CLINT + 0x4000;
const MTIMECMP_HI: usize = CLINT + 0x4004;
const MTIMELOAD_LO: usize = CLINT + 0x4008;
const MTIMECTL: usize = CLINT + 0x4010;
pub(crate) const MTIME_LO: usize = CLINT + 0xBFF8;
const MTIME_HI: usize = CLINT + 0xBFFC;
const MTIP_ID: usize = 7;
const CLIC_USER: usize = 0x20B0_0000; // "CLIC user" window (TRM Table 2.8-1)
// Interrupt matrix fallback for B3: the from-CPU-0 software source routed to TEST_CPU_LINE.
const FROM_CPU_0: usize = 0x500E_5010; // HP_SYSTEM_CPU_INT_FROM_CPU_0_REG
const MAP_FROM_CPU_0: usize = 0x500D_613C; // INTERRUPT_CORE0_CPU_INT_FROM_CPU_0_MAP_REG

// APM (hw_ver1 hp_peri_pms_reg.h): core 0 user-mode allow bits.
const PMS_UM_REG0: usize = 0x500A_5018; // bit 2 L2MEM
const PMS_UM_REG3: usize = 0x500A_5024; // bit 0 HP GPIO
const GPIO_BASE: usize = 0x500E_0000;

/// "C1." on core 1, so its CHECK ids stay apart from the core-0 rows in #604.
fn tag() -> &'static str {
    if csr_read!("mhartid") == 0 { "" } else { "C1." }
}
pub(crate) fn check(id: &str, ok: Option<bool>, detail: core::fmt::Arguments) {
    let verdict = match ok {
        Some(true) => "PASS",
        Some(false) => "FAIL",
        None => "INFO",
    };
    pl!("CHECK {}{id} {verdict} {detail}", tag());
}
pub(crate) fn begin(id: &str) {
    pl!("BEGIN {}{id}", tag());
    hw::drain();
    core1::stage_step();
}
pub(crate) fn uaddr(f: unsafe extern "C" fn()) -> usize {
    f as usize
}
pub(crate) fn show(r: Result<u32, Trap>) -> ShowR {
    ShowR(r)
}
pub(crate) struct ShowR(Result<u32, Trap>);
impl core::fmt::Display for ShowR {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        match self.0 {
            Ok(v) => write!(f, "{v:#010x}"),
            Err(t) => write!(f, "TRAP({t})"),
        }
    }
}
fn unlocked(i: usize) -> bool {
    pmp_cfg_byte(i) & L == 0
}

#[no_mangle]
extern "C" fn pmain() -> ! {
    hw::spin(2_000_000); // let esptool finish reading the MEM_END reply
    pl!("");
    if !CORE0_SUITE {
        core1::launch(PROBE_ID);
    }
    pl!("lichen-probe-p4 {PROBE_ID} (#608), core 0, ROM download-mode state");
    b5_dump();
    setup_pmp();
    b1_user_mode();
    a5_dedicated_gpio();
    b3_interrupt_in_u();
    b3_timer();
    b6_apm();
    b4_clic_from_u();
    b2_locks();
    pl!("END ok");
    hw::drain();
    loop {
        // SAFETY: park; the host hard-resets the badge after END.
        unsafe { core::arch::asm!("wfi", options(nomem, nostack)) };
    }
}

/// Clear the PMP entries the probe owns (this hart's), then grant U its text and stack windows.
pub(crate) fn setup_pmp() {
    let owned = [E_UTEXT, E_UDATA, E_T1, E_T2, E_TOR_LO, E_TOR_HI, E_LOCK_NAPOT, E_COVER_PERI, E_COVER_RAM];
    let mut all_free = true;
    for i in owned {
        if unlocked(i) {
            pmp_off(i);
        } else {
            all_free = false;
            check("SETUP", Some(false), format_args!("pmp entry {i} is locked by the ROM: tests that use it are void"));
        }
    }
    if all_free {
        check("SETUP", None, format_args!("pmp entries 4-7, 10-12, 14 and 15 unlocked and cleared"));
    }
    pmp_set(E_UTEXT, napot(UTEXT, 0x1000), NAPOT | R | X);
    pmp_set(E_UDATA, napot(UDATA, 0x1000), NAPOT | R | W);
}

pub(crate) fn b5_dump() {
    begin("B5");
    pl!("B5 misa={:#010x} mhartid={} mstatus={:#010x}", csr_read!("misa"), csr_read!("mhartid"), csr_read!("mstatus"));
    pl!("B5 marchid={} mimpid={} mtvec={:#010x}", ShowU(csr_try!("0xf12")), ShowU(csr_try!("0xf13")), csr_read!("mtvec"));
    pl!("B5 mtvt(0x307)={} mintstatus(0x346)={} prv(0x810)={}", ShowU(csr_try!("0x307")), ShowU(csr_try!("0x346")), ShowU(csr_try!("0x810")));
    pl!("B5 M-fault path control, unimplemented CSR 0x5c0: {}", ShowU(csr_try!("0x5c0")));
    begin("B5.clic");
    pl!("B5 clic_thresh={} clic_info={}", show(m_lw(CLIC_THRESH)), show(m_lw(0x2080_0004)));
    for j in 0..4 {
        pl!("B5 pmpcfg{j}={:#010x}", hw::pmpcfg_r(j));
    }
    for i in 0..16 {
        pl!("B5 pmp{i:<2} cfg={:#04x} addr={:#010x}", pmp_cfg_byte(i), hw::pmpaddr_r(i));
    }
    macro_rules! pma {
        ($($i:literal: $c:literal $a:literal),*) => {$(
            pl!("B5 pma{:<2} cfg={} addr={}", $i, ShowU(csr_try!($c)), ShowU(csr_try!($a)));
        )*};
    }
    pma!(0: "0xbc0" "0xbd0", 1: "0xbc1" "0xbd1", 2: "0xbc2" "0xbd2", 3: "0xbc3" "0xbd3", 4: "0xbc4" "0xbd4",
         5: "0xbc5" "0xbd5", 6: "0xbc6" "0xbd6", 7: "0xbc7" "0xbd7", 8: "0xbc8" "0xbd8", 9: "0xbc9" "0xbd9",
         10: "0xbca" "0xbda", 11: "0xbcb" "0xbdb", 12: "0xbcc" "0xbdc", 13: "0xbcd" "0xbdd", 14: "0xbce" "0xbde",
         15: "0xbcf" "0xbdf");
    let locked = (0..16).filter(|&i| !unlocked(i)).count();
    check("B5.rom", None, format_args!("core {} after the ROM: {locked} of 16 PMP entries locked (dump above)", csr_read!("mhartid")));
}
struct ShowU(Result<usize, Trap>);
impl core::fmt::Display for ShowU {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        match self.0 {
            Ok(v) => write!(f, "{v:#010x}"),
            Err(t) => write!(f, "TRAP({t})"),
        }
    }
}

pub(crate) fn b1_user_mode() {
    begin("B1");
    let t = user(uaddr(u_ecall), 0, 0, false);
    let ok = t.is_ecall_u() && t.epc == uaddr(u_ecall) && (t.mstatus >> 11) & 3 == 0;
    check("B1.ecall", Some(ok), format_args!("mret MPP=0 then ecall: {t} (expect ecall-U, mepc={:#010x}, MPP=U)", uaddr(u_ecall)));

    let m_prv = csr_try!("0x810");
    let t = user(uaddr(u_csr810), 0, 0, false);
    if t.is_ecall_u() {
        check("B1.prv", Some(t.a0 == 0), format_args!("CSR 0x810 from U reads {:#x} (expect 0); from M {}", t.a0, ShowU(m_prv)));
    } else {
        check("B1.prv", None, format_args!("CSR 0x810 from U traps: {t}; from M {}", ShowU(m_prv)));
    }
    let t = user(uaddr(u_mret), 0, 0, false);
    check("B1.mret", Some(t.code() == 2), format_args!("mret from U: {t} (expect illegal-instr)"));
    let t = user(uaddr(u_csrw_pmp), 0, 0, false);
    check("B1.pmpcsr", Some(t.code() == 2), format_args!("csrw pmpcfg0 from U: {t} (expect illegal-instr)"));

    let mdata = addr_of!(_sbss) as usize;
    let t = user(uaddr(u_load), mdata, 0, false);
    check("B1.deny-r", Some(t.code() == 5 && t.tval == mdata), format_args!("U load of M data {mdata:#x} (no PMP match): {t} (expect load-access-fault)"));
    let t = user(uaddr(u_store), mdata, 0x5a5a_5a5a, false);
    check("B1.deny-w", Some(t.code() == 7 && t.tval == mdata), format_args!("U store to M data: {t} (expect store-access-fault)"));
    let mtext = pmain as extern "C" fn() -> ! as usize;
    let t = user(mtext, 0, 0, false);
    check("B1.deny-x", Some(t.code() == 1 && t.epc == mtext), format_args!("U jump into M text {mtext:#x}: {t} (expect instr-access-fault)"));
    let t = user(uaddr(u_store), UDATA + 0x10, 0x1234_5678, false);
    let back = m_lw(UDATA + 0x10);
    check("B1.grant", Some(t.is_ecall_u() && back == Ok(0x1234_5678)), format_args!("U store inside its RW window: {t}; M reads back {}", show(back)));
}

fn a5_dedicated_gpio() {
    begin("A5");
    for (n, f) in [(0x803, u_csr803 as unsafe extern "C" fn()), (0x804, u_csr804), (0x805, u_csr805)] {
        let t = user(uaddr(f), 0, 0, false);
        if t.is_ecall_u() {
            check("A5", None, format_args!("CSR {n:#x} readable from U: {:#010x} (reads only; a U write could drive routed pads)", t.a0));
        } else {
            check("A5", None, format_args!("CSR {n:#x} from U traps: {t}"));
        }
    }
}

// B3 ------------------------------------------------------------------------------------------------------------------
fn quiesce_test_line() {
    let _ = m_sb(clic_byte(0, TEST_ID, 0), 0);
    let _ = m_sw(FROM_CPU_0, 0);
}
fn quiesce_timer() {
    let _ = m_sb(clic_byte(0, MTIP_ID, 1), 0);
    let _ = m_sw(MTIMECMP_HI, 0xFFFF_FFFF);
    let _ = m_sw(MTIMECMP_LO, 0xFFFF_FFFF);
}
fn mtime() -> Result<u64, Trap> {
    loop {
        let hi = m_lw(MTIME_HI)?;
        let lo = m_lw(MTIME_LO)?;
        if m_lw(MTIME_HI)? == hi {
            return Ok(((hi as u64) << 32) | lo as u64);
        }
    }
}
fn b3_timer() {
    begin("B3t");
    pl!("B3t CLINT msip={} cmp lo/hi={} {} load lo={} ctl={}", show(m_lw(MSIP)), show(m_lw(MTIMECMP_LO)), show(m_lw(MTIMECMP_HI)), show(m_lw(MTIMELOAD_LO)), show(m_lw(MTIMECTL)));
    let (Ok(t0), _) = (mtime(), ()) else {
        check("B3t", Some(false), format_args!("mtime not readable from M: timer case skipped"));
        return;
    };
    hw::spin(100_000);
    let Ok(t1) = mtime() else { return };
    let per = t1.wrapping_sub(t0);
    check("B3t.ticks", Some(per > 0), format_args!("mtime {t0:#x} -> {t1:#x}: {per} ticks per 100k spin loops"));
    if per == 0 {
        return;
    }
    let saved: [Result<u8, Trap>; 4] = core::array::from_fn(|b| m_lbu(clic_byte(0, MTIP_ID, b)));
    pl!("B3t id 7 before: ip/ie/attr/ctl {} {} {} {}", ShowB(saved[0]), ShowB(saved[1]), ShowB(saved[2]), ShowB(saved[3]));
    // SAFETY: single hart; read by the handler only on an interrupt.
    unsafe { write_volatile(addr_of_mut!(hw::IRQ_QUIESCE), quiesce_timer) };
    let _ = m_sb(clic_byte(0, MTIP_ID, 2), 0xC0); // mode M, level-triggered (the CLINT line is level-type)
    let _ = m_sb(clic_byte(0, MTIP_ID, 3), 0xFF);
    let _ = m_sw(CLIC_THRESH, 0);
    let _ = m_lw(CLIC_THRESH);
    let Ok(now) = mtime() else { return };
    let cmp = now + per * 5; // fires about 500k loops in; U spins 2M
    let _ = m_sw(MTIMECMP_HI, 0xFFFF_FFFF);
    let _ = m_sw(MTIMECMP_LO, cmp as u32);
    let _ = m_sw(MTIMECMP_HI, (cmp >> 32) as u32);
    let _ = m_sb(clic_byte(0, MTIP_ID, 1), 1);
    set_irq_count(0);
    let t = user(uaddr(u_spin), 2_000_000, 0, false);
    let n = irq_count();
    // SAFETY: written by the handler before its mret.
    let last = unsafe { read_volatile(addr_of!(hw::IRQ_LAST)) };
    let in_u = (last.mstatus >> 11) & 3 == 0 && (last.epc & !0xFFF) == UTEXT;
    check("B3t.preempt", Some(n == 1 && in_u && t.is_ecall_u() && t.a0 == 0),
        format_args!("CLINT timer while U spins: {n} interrupt(s), last {last}, MPP {}; U ended {t}", (last.mstatus >> 11) & 3));
    quiesce_timer();
    for b in [2, 3] {
        if let Ok(v) = saved[b] {
            let _ = m_sb(clic_byte(0, MTIP_ID, b), v);
        }
    }
}
pub(crate) fn b3_interrupt_in_u() {
    begin("B3");
    let saved: [Result<u8, Trap>; 4] = core::array::from_fn(|b| m_lbu(clic_byte(0, TEST_ID, b)));
    let thresh = m_lw(CLIC_THRESH);
    pl!("B3 line {TEST_ID} before: ip/ie/attr/ctl {} {} {} {}, thresh {}", ShowB(saved[0]), ShowB(saved[1]), ShowB(saved[2]), ShowB(saved[3]), show(thresh));
    // SAFETY: single hart; the handler reads this pointer only on an interrupt.
    unsafe { write_volatile(addr_of_mut!(hw::IRQ_QUIESCE), quiesce_test_line) };
    let _ = m_sb(clic_byte(0, TEST_ID, 1), 0);
    let _ = m_sb(clic_byte(0, TEST_ID, 2), 0xC2); // mode M, positive edge, non-vectored (-> mtvec)
    let _ = m_sb(clic_byte(0, TEST_ID, 3), 0xFF); // highest level
    let _ = m_sw(CLIC_THRESH, 0);
    let _ = m_lw(CLIC_THRESH); // the threshold takes effect after a read-back (interrupt_clic.h)
    let _ = m_sb(clic_byte(0, TEST_ID, 1), 1);

    // Source 1: the CLIC pending bit, set by software.
    let _ = m_sb(clic_byte(0, TEST_ID, 0), 1);
    let ip = m_lbu(clic_byte(0, TEST_ID, 0));
    let mut source = "clic-ip";
    if ip != Ok(1) && csr_read!("mhartid") != 0 {
        // The fallback below drives core 0's interrupt matrix: no use on core 1.
        check("B3", Some(false), format_args!("clic-ip not settable on this core (ip read {}): B3 skipped", ShowB(ip)));
        let _ = m_sb(clic_byte(0, TEST_ID, 1), 0);
        return;
    }
    if ip != Ok(1) {
        // Source 2: the interrupt matrix, from-CPU-0 routed to the test line.
        source = "from-cpu-0";
        let _ = m_sb(clic_byte(0, TEST_ID, 0), 0);
        let _ = m_sw(MAP_FROM_CPU_0, TEST_CPU_LINE as u32);
        let _ = m_sw(FROM_CPU_0, 1);
    }
    hw::spin(1000);
    let held = irq_count();
    check("B3.m-masked", Some(held == 0), format_args!("pending via {source} (ip read {}), M with MIE=0 takes no interrupt: count {held}", ShowB(ip)));

    let mut result = None;
    for mpie in [false, true] {
        set_irq_count(0);
        let t = user(uaddr(u_spin), 200_000, 0, mpie);
        let n = irq_count();
        // SAFETY: written by the handler before its mret; read after the U run ended.
        let last = unsafe { read_volatile(addr_of!(hw::IRQ_LAST)) };
        pl!("B3 mpie={} U spin ended {t}, interrupts {n}, last {last}, MPP at irq {}", mpie as u8, (last.mstatus >> 11) & 3);
        if n > 0 {
            let in_u = (last.mstatus >> 11) & 3 == 0 && (last.epc & !0xFFF) == UTEXT;
            let resumed = t.is_ecall_u() && t.a0 == 0;
            result = Some((mpie, n, in_u, resumed, last));
            break;
        }
        // Re-arm for the second try.
        let _ = m_sb(clic_byte(0, TEST_ID, 0), 1);
        let _ = m_sw(FROM_CPU_0, if source == "from-cpu-0" { 1 } else { 0 });
    }
    match result {
        Some((mpie, n, in_u, resumed, last)) => check("B3.irq-in-u", Some(in_u && resumed && n == 1),
            format_args!("via {source}, MPIE={}: {n} interrupt(s) taken from U (mepc {:#x}), U resumed and finished: {resumed}", mpie as u8, last.epc)),
        None => check("B3.irq-in-u", Some(false), format_args!("via {source}: no interrupt taken while in U (MPIE 0 and 1)")),
    }
    // Restore the line and the threshold.
    let _ = m_sb(clic_byte(0, TEST_ID, 1), 0);
    quiesce_test_line();
    for b in [2, 3] {
        if let Ok(v) = saved[b] {
            let _ = m_sb(clic_byte(0, TEST_ID, b), v);
        }
    }
    if let Ok(v) = thresh {
        let _ = m_sw(CLIC_THRESH, v);
    }
}
fn irq_count() -> u32 {
    // SAFETY: single hart; see hw.
    unsafe { read_volatile(addr_of!(hw::IRQ_COUNT)) }
}
fn set_irq_count(v: u32) {
    // SAFETY: single hart; see hw.
    unsafe { write_volatile(addr_of_mut!(hw::IRQ_COUNT), v) }
}
struct ShowB(Result<u8, Trap>);
impl core::fmt::Display for ShowB {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        match self.0 {
            Ok(v) => write!(f, "{v:#04x}"),
            Err(t) => write!(f, "TRAP({t})"),
        }
    }
}

// B6 ------------------------------------------------------------------------------------------------------------------
fn b6_apm() {
    begin("B6");
    let reg0 = m_lw(PMS_UM_REG0);
    let reg3 = m_lw(PMS_UM_REG3);
    pl!("B6 PMS core0 UM reg0={} reg3={}", show(reg0), show(reg3));
    let (Ok(reg0), Ok(reg3)) = (reg0, reg3) else {
        check("B6", Some(false), format_args!("PMS registers not readable from M: APM checks void"));
        return;
    };

    // GPIO: the PMP grants U read on the GPIO page; the APM UM bit decides.
    pmp_set(E_T1, napot(GPIO_BASE, 0x1000), NAPOT | R);
    let m_before = m_lw(GPIO_BASE);
    let u_allow = user(uaddr(u_load), GPIO_BASE, 0, false);
    check("B6.gpio-ctl", Some(m_before.is_ok() && u_allow.is_ecall_u()),
        format_args!("control, UM bit allow: M read {}, U read {u_allow}", show(m_before)));
    let _ = m_sw(PMS_UM_REG3, reg3 & !1);
    let cleared = m_lw(PMS_UM_REG3);
    begin("B6.gpio");
    let u_deny = user(uaddr(u_load), GPIO_BASE, 0, false);
    let m_after = m_lw(GPIO_BASE);
    check("B6.gpio", Some(u_deny.code() == 5 && m_after.is_ok()),
        format_args!("UM HP_GPIO cleared (reg3 now {}): U read {u_deny} (expect fault), M read {}", show(cleared), show(m_after)));
    let _ = m_sw(PMS_UM_REG3, reg3);
    let u_back = user(uaddr(u_load), GPIO_BASE, 0, false);
    check("B6.gpio-restore", Some(u_back.is_ecall_u()), format_args!("UM bit restored: U read {u_back} (bus alive)"));
    pmp_off(E_T1);

    // L2MEM: uncached alias (APM-checked per C3) vs cached address (PMP only).
    let pattern = 0xC0FF_EE00;
    let w = m_sw(SCRATCH_UNCACHED, pattern);
    let m_un = m_lw(SCRATCH_UNCACHED);
    pl!("B6 M write via uncached alias {w:?}, read back {}", show(m_un));
    pmp_set(E_T1, napot(SCRATCH_UNCACHED, 0x1000), NAPOT | R);
    pmp_set(E_T2, napot(SCRATCH, 0x1000), NAPOT | R);
    let u_un = user(uaddr(u_load), SCRATCH_UNCACHED, 0, false);
    let u_ca = user(uaddr(u_load), SCRATCH, 0, false);
    check("B6.l2mem-ctl", Some(u_un.is_ecall_u() && u_ca.is_ecall_u()),
        format_args!("control, UM bit allow: U uncached {u_un} a0={:#x}; U cached {u_ca}", u_un.a0));
    begin("B6.l2mem");
    let _ = m_sw(PMS_UM_REG0, reg0 & !(1 << 2));
    let cleared = m_lw(PMS_UM_REG0);
    let u_un = user(uaddr(u_load), SCRATCH_UNCACHED, 0, false);
    let u_ca = user(uaddr(u_load), SCRATCH, 0, false);
    let m_un = m_lw(SCRATCH_UNCACHED);
    let _ = m_sw(PMS_UM_REG0, reg0);
    check("B6.l2mem-uncached", Some(u_un.code() == 5 && m_un.is_ok()),
        format_args!("UM L2MEM cleared (reg0 now {}): U uncached {u_un} (expect fault), M uncached {}", show(cleared), show(m_un)));
    check("B6.l2mem-cached", None,
        format_args!("UM L2MEM cleared: U cached {u_ca} (C3 predicts allowed: APM does not see cached accesses)"));
    pmp_off(E_T1);
    pmp_off(E_T2);
}

// B4 ------------------------------------------------------------------------------------------------------------------
/// U writes `val` (byte or word) to `addr`; M reads before and after, then restores. Reports fault / ignored / effect.
fn u_write_probe(id: &str, what: &str, addr: usize, val: u32, byte: bool) {
    let before = if byte { m_lbu(addr).map(u32::from) } else { m_lw(addr) };
    let Ok(old) = before else {
        check(id, None, format_args!("{what} {addr:#x}: M read traps {} -> skipped", show(before)));
        return;
    };
    let ctl = user(uaddr(u_load), addr & !3, 0, false);
    pl!("{id} control: U read of {:#x} with the PMP grant: {ctl} a0={:#x}", addr & !3, ctl.a0);
    let stub = if byte { u_storeb as unsafe extern "C" fn() } else { u_store };
    let t = user(uaddr(stub), addr, val as usize, false);
    let after = if byte { m_lbu(addr).map(u32::from) } else { m_lw(addr) };
    let effect = match (t.is_ecall_u(), after) {
        (false, _) => "U write FAULTED (privilege-checked)",
        (true, Ok(a)) if a == old => "U write IGNORED (value unchanged)",
        (true, Ok(_)) => "U write TOOK EFFECT",
        (true, Err(_)) => "M read-back traps",
    };
    let _ = if byte { m_sb(addr, old as u8) } else { m_sw(addr, old) };
    check(id, None, format_args!("{what} {addr:#x}: before {old:#x}, U wrote {val:#x}: {t}; after {}: {effect}", show(after)));
}
fn b4_clic_from_u() {
    begin("B4");
    // Control: a small 4 KB grant on the CLIC page alone (the big NAPOT below is the bootloader's shape).
    pmp_set(E_T2, napot(0x2080_0000, 0x1000), NAPOT | R | W);
    let t = user(uaddr(u_load), CLIC_THRESH, 0, false);
    check("B4.grant4k", None, format_args!("U read of the CLIC threshold with a 4 KB NAPOT RW grant: {t}"));
    pmp_off(E_T2);
    // The v6.1 bootloader's locked entry 0 (NAPOT 0x2000_0000-0x3000_0000 RW), mimicked unlocked for U.
    pmp_set(E_T1, napot(0x2000_0000, 0x1000_0000), NAPOT | R | W);
    u_write_probe("B4.thresh", "CLIC threshold", CLIC_THRESH, 0xFF00_0000, false);
    u_write_probe("B4.ie", "CLIC ie line 41", clic_byte(0, TEST_ID, 1), 1, true);
    u_write_probe("B4.attr", "CLIC attr line 41", clic_byte(0, TEST_ID, 2), 0x02, true);
    u_write_probe("B4.ctl", "CLIC ctl line 41", clic_byte(0, TEST_ID, 3), 0xFF, true);
    u_write_probe("B4.ie7", "CLIC ie id 7 (MTIP)", clic_byte(0, MTIP_ID, 1), 1, true);
    u_write_probe("B4.msip", "CLINT msip", MSIP, 1, false);
    u_write_probe("B4.mtimecmp", "CLINT mtimecmp lo", MTIMECMP_LO, 0, false);
    u_write_probe("B4.mtimeload", "CLINT mtimeload lo", MTIMELOAD_LO, 0x1234, false);
    u_write_probe("B4.mtimectl", "CLINT mtimectl (MTIME_EN)", MTIMECTL, 0, false);
    let t = user(uaddr(u_load), MTIME_LO, 0, false);
    check("B4.mtime-u", None, format_args!("U read of mtime lo: {t} a0={:#x}", t.a0));
    begin("B4.clic-user");
    pl!("B4 CLIC user window M reads: +0 {} +4 {} +8 {}", show(m_lw(CLIC_USER)), show(m_lw(CLIC_USER + 4)), show(m_lw(CLIC_USER + 8)));
    u_write_probe("B4.clic-user", "CLIC user window +8", CLIC_USER + 8, 0xFF00_0000, false);
    // The other core's windows (CLIC_THRESH + OTHER_CORE, ...) are not probed: with core 1 held off in ROM state
    // an M read of them stalled the bus until reset (#608 run 1, probe ed277b62).
    pmp_off(E_T1);
}

// B2 (last) -------------------------------------------------------------------------------------------------------------
fn b2_locks() {
    begin("B2");
    if ![E_TOR_LO, E_TOR_HI, E_LOCK_NAPOT, E_COVER_PERI, E_COVER_RAM].iter().all(|&i| unlocked(i)) {
        check("B2", Some(false), format_args!("entries 10-12, 14, 15 not free: skipped"));
        return;
    }
    let _ = m_sw(LOCK_A, 0x1111_1111);
    let _ = m_sw(LOCK_B, 0x2222_2222);
    // Run 2 hung right after locking one R-only entry: if v1 denies M every unmatched address once any entry is
    // locked, M needs covers first. Locked covers sit above the test entries (lowest index wins).
    pmp_set(E_COVER_RAM, napot(0x4FF0_0000, 0x8_0000), NAPOT | R | W | X | L);
    pmp_set(E_COVER_PERI, napot(0x5000_0000, 0x10_0000), NAPOT | R | W | L);
    pl!("B2 covers locked: entry {E_COVER_RAM} L2MEM 512 KB RWX, entry {E_COVER_PERI} HP peripherals 1 MB RW");
    hw::drain();
    // a) a locked R-only NAPOT entry: M read, U read and U write.
    pmp_set(E_LOCK_NAPOT, napot(LOCK_A, 0x100), NAPOT | R | L);
    let m_r = m_lw(LOCK_A);
    check("B2.lock-m-read", Some(m_r == Ok(0x1111_1111)), format_args!("locked R entry {E_LOCK_NAPOT} (cfg {:#04x}): M read {}", pmp_cfg_byte(E_LOCK_NAPOT), show(m_r)));
    let u_r = user(uaddr(u_load), LOCK_A, 0, false);
    let u_w = user(uaddr(u_store), LOCK_A, 0x3333_3333, false);
    check("B2.lock-grants-u", Some(u_r.is_ecall_u() && u_w.code() == 7),
        format_args!("locked R entry, no other U grant: U read {u_r}; U write {u_w}"));
    // b) writes to a locked entry are ignored.
    let addr0 = hw::pmpaddr_r(E_LOCK_NAPOT);
    let cfg0 = pmp_cfg_byte(E_LOCK_NAPOT);
    hw::pmpaddr_w(E_LOCK_NAPOT, napot(LOCK_B, 0x100));
    hw::pmp_cfg_set(E_LOCK_NAPOT, NAPOT | R | W | X);
    let (addr1, cfg1) = (hw::pmpaddr_r(E_LOCK_NAPOT), pmp_cfg_byte(E_LOCK_NAPOT));
    check("B2.lock-sticky", Some(addr1 == addr0 && cfg1 == cfg0),
        format_args!("rewrite locked entry: addr {addr0:#x} -> {addr1:#x}, cfg {cfg0:#04x} -> {cfg1:#04x} (expect unchanged)"));
    // c) a locked TOR entry i and the pmpaddr[i-1] it uses as its base.
    hw::pmpaddr_w(E_TOR_LO, LOCK_B >> 2);
    pmp_set(E_TOR_HI, (LOCK_B + 0x100) >> 2, TOR | R | L);
    let lo0 = hw::pmpaddr_r(E_TOR_LO);
    hw::pmpaddr_w(E_TOR_LO, (LOCK_B + 0x80) >> 2);
    let lo1 = hw::pmpaddr_r(E_TOR_LO);
    check("B2.tor", None, format_args!("locked TOR entry {E_TOR_HI}: pmpaddr{E_TOR_LO} {lo0:#x} -> {lo1:#x} after a write ({})",
        if lo1 == lo0 { "locked with i" } else { "still writable" }));
    let u_tor = user(uaddr(u_store), LOCK_B + 0x10, 0x4444_4444, false);
    check("B2.tor-u", Some(u_tor.code() == 7), format_args!("U write inside the locked R TOR range: {u_tor}"));
    // d) with locked entries present, is an address no entry matches still open to M? (uncached L2MEM alias)
    begin("B2.unmatched");
    let m_un = m_lw(SCRATCH_UNCACHED);
    check("B2.unmatched-m", None, format_args!("M read of unmatched {SCRATCH_UNCACHED:#x} with locked entries present: {}", show(m_un)));
    // e) last: an M write the locked R entry forbids. Runs 4 and 5 stopped here with no trap delivered.
    begin("B2.m-write");
    pl!("CHECK B2.m-write INFO an M store to the locked R entry follows; no CHECK line after this means the core hung (no trap)");
    hw::drain();
    let m_w = m_sw(LOCK_A, 0xdead_beef);
    check("B2.m-write", Some(matches!(m_w, Err(t) if t.code() == 7)), format_args!("M store to the locked R entry: {:?}", m_w.err()));
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    pl!("FATAL panic: {info}");
    pl!("END fatal");
    hw::drain();
    loop {}
}
