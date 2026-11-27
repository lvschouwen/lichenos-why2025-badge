//! #608 bench probe for the #604 isolation matrix: B5, B1, A5, B3, B6, B4 and B2 (last: locks hold until reset)
//! on core 0 of the ESP32-P4 rev v1.0, in ROM download-mode state, loaded by `esptool load-ram`.
//! Output: one `CHECK <id> <PASS|FAIL|INFO> ...` line per result, `BEGIN <id>` before each risky step, `END` last.
#![no_std]
#![no_main]

mod hw;

use core::arch::global_asm;
use core::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};
use hw::{csr_read, csr_try, m_lbu, m_lw, m_sb, m_sw, napot, pmp_cfg_byte, pmp_off, pmp_set, user, Trap};
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

// PMP entries the probe owns (cleared first, skipped when the ROM left them locked).
const E_UTEXT: usize = 4;
const E_UDATA: usize = 5;
const E_T1: usize = 6;
const E_T2: usize = 7;
const E_LOCK_NAPOT: usize = 12;
const E_TOR_LO: usize = 13;
const E_TOR_HI: usize = 14;

const UTEXT: usize = 0x4FF1_8000;
const UDATA: usize = 0x4FF5_8000;
const SCRATCH: usize = 0x4FF5_A000; // L2MEM test buffer (4 KB, cached alias)
const SCRATCH_UNCACHED: usize = SCRATCH + 0x4000_0000; // soc.h:223-224 uncached alias
const LOCK_A: usize = 0x4FF5_B000; // B2 NAPOT, 256 B
const LOCK_B: usize = 0x4FF5_B800; // B2 TOR, 256 B

// CLIC (clic_reg.h): per-core window, other core at +0x10000; line i control word at CTRL + 4 * i.
const CLIC_THRESH: usize = 0x2080_0008;
const CLIC_CTRL: usize = 0x2080_1000;
const TEST_CPU_LINE: usize = 25;
const TEST_ID: usize = 16 + TEST_CPU_LINE; // CLIC_EXT_INTR_NUM_OFFSET
const fn clic_byte(core_off: usize, id: usize, byte: usize) -> usize {
    CLIC_CTRL + core_off + 4 * id + byte // 0 ip, 1 ie, 2 attr, 3 ctl
}
// Interrupt matrix fallback for B3: the from-CPU-0 software source routed to TEST_CPU_LINE.
const FROM_CPU_0: usize = 0x500E_5010; // HP_SYSTEM_CPU_INT_FROM_CPU_0_REG
const MAP_FROM_CPU_0: usize = 0x500D_613C; // INTERRUPT_CORE0_CPU_INT_FROM_CPU_0_MAP_REG

// APM (hw_ver1 hp_peri_pms_reg.h): core 0 user-mode allow bits.
const PMS_UM_REG0: usize = 0x500A_5018; // bit 2 L2MEM
const PMS_UM_REG3: usize = 0x500A_5024; // bit 0 HP GPIO
const GPIO_BASE: usize = 0x500E_0000;

fn check(id: &str, ok: Option<bool>, detail: core::fmt::Arguments) {
    let verdict = match ok {
        Some(true) => "PASS",
        Some(false) => "FAIL",
        None => "INFO",
    };
    pl!("CHECK {id} {verdict} {detail}");
}
fn begin(id: &str) {
    pl!("BEGIN {id}");
    hw::drain();
}
fn uaddr(f: unsafe extern "C" fn()) -> usize {
    f as usize
}
fn show(r: Result<u32, Trap>) -> ShowR {
    ShowR(r)
}
struct ShowR(Result<u32, Trap>);
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
    pl!("lichen-probe-p4 {PROBE_ID} (#608), core 0, ROM download-mode state");
    b5_dump();
    let owned = [E_UTEXT, E_UDATA, E_T1, E_T2, E_LOCK_NAPOT, E_TOR_LO, E_TOR_HI];
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
        check("SETUP", None, format_args!("pmp entries 4-7 and 12-14 unlocked and cleared"));
    }
    pmp_set(E_UTEXT, napot(UTEXT, 0x1000), NAPOT | R | X);
    pmp_set(E_UDATA, napot(UDATA, 0x1000), NAPOT | R | W);
    b1_user_mode();
    a5_dedicated_gpio();
    b3_interrupt_in_u();
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

fn b5_dump() {
    begin("B5");
    pl!("B5 misa={:#010x} mhartid={} mstatus={:#010x}", csr_read!("misa"), csr_read!("mhartid"), csr_read!("mstatus"));
    pl!("B5 marchid={} mimpid={} mtvec={:#010x}", ShowU(csr_try!("0xf12")), ShowU(csr_try!("0xf13")), csr_read!("mtvec"));
    pl!("B5 mtvt(0x307)={} mintstatus(0x346)={} prv(0x810)={}", ShowU(csr_try!("0x307")), ShowU(csr_try!("0x346")), ShowU(csr_try!("0x810")));
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
    check("B5.rom", None, format_args!("core 0 after the ROM loader: {locked} of 16 PMP entries locked (dump above)"));
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

fn b1_user_mode() {
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
fn b3_interrupt_in_u() {
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
    // The v6.1 bootloader's locked entry 0 (NAPOT 0x2000_0000-0x3000_0000 RW), mimicked unlocked for U.
    pmp_set(E_T1, napot(0x2000_0000, 0x1000_0000), NAPOT | R | W);
    u_write_probe("B4.thresh", "CLIC threshold", CLIC_THRESH, 0xFF00_0000, false);
    u_write_probe("B4.ie", "CLIC ie line 41", clic_byte(0, TEST_ID, 1), 1, true);
    u_write_probe("B4.attr", "CLIC attr line 41", clic_byte(0, TEST_ID, 2), 0x02, true);
    u_write_probe("B4.ctl", "CLIC ctl line 41", clic_byte(0, TEST_ID, 3), 0xFF, true);
    for (name, off) in hw_clint::CANDIDATES {
        u_write_probe("B4.clint", name, 0x2000_0000 + off, 0x0, false);
    }
    // The other core's windows (CLIC_THRESH + OTHER_CORE, ...) are not probed: with core 1 held off in ROM state
    // an M read of them stalled the bus until reset (#608 run 1, probe ed277b62).
    pmp_off(E_T1);
}
mod hw_clint {
    /// CLINT words to probe (offset from 0x2000_0000): filled from the TRM table once known; empty = not probed.
    pub const CANDIDATES: &[(&str, usize)] = &[];
}

// B2 (last) -------------------------------------------------------------------------------------------------------------
fn b2_locks() {
    begin("B2");
    if !(unlocked(E_LOCK_NAPOT) && unlocked(E_TOR_LO) && unlocked(E_TOR_HI)) {
        check("B2", Some(false), format_args!("entries 12-14 not free: skipped"));
        return;
    }
    let _ = m_sw(LOCK_A, 0x1111_1111);
    let _ = m_sw(LOCK_B, 0x2222_2222);
    // a) a locked R-only NAPOT entry binds M and grants U.
    pmp_set(E_LOCK_NAPOT, napot(LOCK_A, 0x100), NAPOT | R | L);
    let m_r = m_lw(LOCK_A);
    let m_w = m_sw(LOCK_A, 0xdead_beef);
    check("B2.lock-binds-m", Some(m_r == Ok(0x1111_1111) && matches!(m_w, Err(t) if t.code() == 7)),
        format_args!("locked R entry: M read {}, M write {:?}", show(m_r), m_w.err().map(|t| t.code())));
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
    let m_w = m_sw(LOCK_B, 0xdead_beef);
    let lo0 = hw::pmpaddr_r(E_TOR_LO);
    hw::pmpaddr_w(E_TOR_LO, (LOCK_B + 0x80) >> 2);
    let lo1 = hw::pmpaddr_r(E_TOR_LO);
    check("B2.tor", None, format_args!("locked TOR entry {E_TOR_HI}: M write {:?} (expect 7); pmpaddr{E_TOR_LO} {lo0:#x} -> {lo1:#x} after a write ({})",
        m_w.err().map(|t| t.code()), if lo1 == lo0 { "locked with i" } else { "still writable" }));
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    pl!("FATAL panic: {info}");
    pl!("END fatal");
    hw::drain();
    loop {}
}
