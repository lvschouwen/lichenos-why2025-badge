//! #608 run 9, core 0: B6 leftovers.
//! - The mtime frequency against SYSTIMER unit 0 (fixed XTAL/2.5 = 16 MHz, esp_hw_support/port/esp32p4/systimer.c),
//!   with the UART at 115200 8N1 as a third, independent clock (10 bits per byte).
//! - A "PMS violation record": hw_ver1 HP_PERI_PMS has only allow bits and IDF's P4 APM HAL has no violation API, so
//!   the candidates are the AXI ICM address-hole records (icm_sys_reg.h), read before and after U APM faults.

use crate::hw::{self, m_lw, m_sw, napot, pmp_off, pmp_set, user, Trap, NAPOT, R};
use crate::{begin, check, show, uaddr, E_T1, GPIO_BASE, MTIME_HI, MTIME_LO, PMS_UM_REG0, PMS_UM_REG3, SCRATCH_UNCACHED};

// hw_ver1 systimer_reg.h (DR_REG_SYSTIMER_BASE 0x500E_2000)
const ST_CONF: usize = 0x500E_2000;
const ST_UNIT0_OP: usize = 0x500E_2004; // bit 30 UPDATE, bit 29 VALUE_VALID
const ST_UNIT0_HI: usize = 0x500E_2040;
const ST_UNIT0_LO: usize = 0x500E_2044;
const ST_HZ: u64 = 16_000_000;
// hw_ver1 hp_sys_clkrst_reg.h: SOC_CLK_CTRL2 bit 23 SYSTIMER_APB_CLK_EN; PERI_CLK_CTRL21 bit 29 SRC_SEL, bit 30 CLK_EN
const CLK_CTRL2: usize = 0x500E_601C;
const PERI_CLK_CTRL21: usize = 0x500E_6098;
// hw_ver1 icm_sys_reg.h (DR_REG_AXI_ICM_BASE 0x500A_4000)
const ICM: [(&str, usize); 8] = [
    ("date", 0x500A_4000),
    ("clk_en", 0x500A_4004),
    ("dlock_st", 0x500A_4008),
    ("int_raw", 0x500A_400C),
    ("sys_hole_addr", 0x500A_4038),
    ("sys_hole_info", 0x500A_403C),
    ("cpu_hole_addr", 0x500A_4040),
    ("cpu_hole_info", 0x500A_4044),
];
const BAUD: u64 = 115_200;

fn systimer() -> Option<u64> {
    m_sw(ST_UNIT0_OP, 1 << 30).ok()?;
    for _ in 0..10_000 {
        if m_lw(ST_UNIT0_OP).ok()? & (1 << 29) != 0 {
            let hi = m_lw(ST_UNIT0_HI).ok()? as u64;
            let lo = m_lw(ST_UNIT0_LO).ok()? as u64;
            return Some((hi << 32) | lo);
        }
    }
    None
}
fn mtime() -> Option<u64> {
    loop {
        let hi = m_lw(MTIME_HI).ok()?;
        let lo = m_lw(MTIME_LO).ok()?;
        if m_lw(MTIME_HI).ok()? == hi {
            return Some(((hi as u64) << 32) | lo as u64);
        }
    }
}
fn both() -> Option<(u64, u64)> {
    Some((systimer()?, mtime()?))
}
/// Hz to kHz with three decimals, integer only.
struct Khz(u64);
impl core::fmt::Display for Khz {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        write!(f, "{}.{:03} kHz", self.0 / 1000, self.0 % 1000)
    }
}

pub fn timers() {
    begin("B6.mtime");
    pl!("B6 systimer conf={} soc_clk_ctrl2={} peri_clk_ctrl21={}", show(m_lw(ST_CONF)), show(m_lw(CLK_CTRL2)), show(m_lw(PERI_CLK_CTRL21)));
    let Some((s0, m0)) = both() else {
        check("B6.mtime", Some(false), format_args!("systimer or mtime not readable: skipped"));
        return;
    };
    hw::spin(2_000_000);
    let Some((s1, m1)) = both() else { return };
    let (ds, dm) = (s1.wrapping_sub(s0), m1.wrapping_sub(m0));
    if ds == 0 {
        check("B6.mtime", Some(false), format_args!("systimer unit 0 does not count (conf above): skipped"));
        return;
    }
    let mtime_hz = dm * ST_HZ / ds;
    check("B6.mtime", None, format_args!("over 2M spin loops: systimer +{ds}, mtime +{dm}: mtime = {} if systimer is 16 MHz (ratio mtime/systimer x1000 = {})",
        Khz(mtime_hz), dm * 1000 / ds));

    // UART as the third clock: 12 lines of 96 bytes = 1152 bytes = 100.000 ms at 115200 8N1, from an empty FIFO to an
    // empty FIFO (the last byte's own 87 us in the shift register is not counted, so expect ~99.9 ms).
    hw::drain();
    let Some((s0, m0)) = both() else { return };
    for _ in 0..12 {
        pl!("{:.<94}", "B6 uart clock line ");
    }
    while hw::txfifo_count() != 0 {}
    let Some((s1, m1)) = both() else { return };
    let (ds, dm) = (s1.wrapping_sub(s0), m1.wrapping_sub(m0));
    let bytes = 12 * 96u64;
    // f = ticks / (bytes * 10 / BAUD), the last byte is still shifting out: use bytes - 1.
    let st_hz = ds * BAUD / ((bytes - 1) * 10);
    let mt_hz = dm * BAUD / ((bytes - 1) * 10);
    check("B6.mtime-uart", None, format_args!("{} bytes at {BAUD} baud: systimer +{ds} = {}, mtime +{dm} = {} (UART-timed)",
        bytes - 1, Khz(st_hz), Khz(mt_hz)));
}

fn icm(when: &str) {
    pl!("B6 ICM {when}: {}={} {}={} {}={} {}={}", ICM[0].0, show(m_lw(ICM[0].1)), ICM[1].0, show(m_lw(ICM[1].1)),
        ICM[2].0, show(m_lw(ICM[2].1)), ICM[3].0, show(m_lw(ICM[3].1)));
    pl!("B6 ICM {when}: {}={} {}={} {}={} {}={}", ICM[4].0, show(m_lw(ICM[4].1)), ICM[5].0, show(m_lw(ICM[5].1)),
        ICM[6].0, show(m_lw(ICM[6].1)), ICM[7].0, show(m_lw(ICM[7].1)));
}
const ICM_INT_CLR: usize = 0x500A_4018; // bit 2 CPU_ADDRHOLE
fn rec() -> (Result<u32, Trap>, Result<u32, Trap>, Result<u32, Trap>) {
    (m_lw(ICM[3].1), m_lw(ICM[6].1), m_lw(ICM[7].1))
}
fn rec_line(step: &str) {
    let (raw, addr, info) = rec();
    pl!("B6 record after {step}: int_raw={} cpu_hole_addr={} cpu_hole_info={}", show(raw), show(addr), show(info));
}
fn clear_record() {
    let _ = m_sw(ICM_INT_CLR, 1 << 2);
}
/// U load of `addr` with a 4 KB PMP read grant while `bit` of the PMS UM register `reg` is cleared.
fn u_apm_fault(reg: usize, saved: u32, bit: u32, addr: usize) -> Trap {
    pmp_set(E_T1, napot(addr & !0xFFF, 0x1000), NAPOT | R);
    let _ = m_sw(reg, saved & !(1 << bit));
    let t = user(uaddr(crate::u_load), addr, 0, false);
    let _ = m_sw(reg, saved);
    pmp_off(E_T1);
    t
}

pub fn violation_record() {
    begin("B6.record");
    icm("before");
    let (Ok(reg0), Ok(reg3)) = (m_lw(PMS_UM_REG0), m_lw(PMS_UM_REG3)) else {
        check("B6.record", Some(false), format_args!("PMS registers not readable: skipped"));
        return;
    };
    let t = u_apm_fault(PMS_UM_REG3, reg3, 0, GPIO_BASE);
    rec_line("U GPIO fault at +0");
    let first = rec();
    let t2 = u_apm_fault(PMS_UM_REG3, reg3, 0, GPIO_BASE + 4);
    rec_line("second U GPIO fault at +4, no clear");
    let second = rec();
    check("B6.record-gpio", Some(first.1 == Ok(GPIO_BASE as u32) && t.code() == 5),
        format_args!("U GPIO {t}: record addr {}; second fault {t2}: {}", show(first.1),
            if second.1 == first.1 { "record LATCHED (first kept)" } else { "record OVERWRITTEN" }));
    clear_record();
    rec_line("int_clr bit 2");
    let cleared = rec();
    check("B6.record-clear", None, format_args!("after ICM_INT_CLR bit 2: int_raw {}, addr {} ({})", show(cleared.0), show(cleared.1),
        if cleared.1 == first.1 { "addr kept, only the flag clears" } else { "addr reset" }));
    let t3 = u_apm_fault(PMS_UM_REG0, reg0, 2, SCRATCH_UNCACHED);
    rec_line("U uncached L2MEM fault");
    let l2 = rec();
    check("B6.record-l2mem", None, format_args!("U uncached L2MEM {t3}: record {}",
        if l2 == cleared { "unchanged (L2MEM APM not reported through the ICM)" } else { "CHANGED" }));
    clear_record();
    let before = rec();
    let t4 = user(uaddr(crate::u_load), crate::SCRATCH, 0, false);
    let pmp_only = rec();
    check("B6.record-pmp", None, format_args!("U load with no PMP grant {t4}: record {}",
        if pmp_only == before { "unchanged (PMP faults stay inside the core)" } else { "CHANGED" }));
    clear_record();
}
