//! #608 run 8: core 0 starts core 1 with IDF's start_other_core sequence (hw_ver1 registers) and then only watches
//! a stage word; core 1 runs the tests and is the only hart that prints or traps until it reports done.
//! A core-1 hang therefore shows as "silent at stage N" from core 0, which also tells whether core 0 survived it.

use crate::hw::{self, m_lw, m_sw, napot, pmp_off, pmp_set, user, NAPOT, R, W};
use crate::{begin, check, show, uaddr, CLIC_THRESH, CLINT, E_T1, E_T2, MTIME_LO};
use core::sync::atomic::{AtomicU32, Ordering::SeqCst};

// hw_ver1 reg_base.h / hp_sys_clkrst_reg.h / hp_system_reg.h / pmu_reg.h
const CLK_CTRL0: usize = 0x500E_6014; // HP_SYS_CLKRST_SOC_CLK_CTRL0_REG: bit 1 CORE1_CLIC_CLK_EN, bit 4 CORE1_CPU_CLK_EN
const RST_EN0: usize = 0x500E_60C0; // HP_SYS_CLKRST_HP_RST_EN0_REG: bit 8 RST_EN_CORE1_GLOBAL
const CORESTALLED_ST: usize = 0x500E_5064; // HP_SYSTEM_CPU_CORESTALLED_ST_REG: bit 1 core 1 stalled
const PMU_SW_STALL: usize = 0x5011_5200; // PMU_CPU_SW_STALL_REG: [23:16] hpcore1_stall_code (0x86 stall, 0xFF run)
const ETS_SET_APPCPU_BOOT_ADDR: usize = 0x4FC0_00A8; // esp32p4.rom.ld

const DONE: u32 = 0xD0E;
/// Core 1's progress: 0 not started, then one step per BEGIN, DONE when its output is drained.
static STAGE: AtomicU32 = AtomicU32::new(0);
/// Watchdog: polls of spin(10_000) with no stage change before core 0 calls core 1 silent (~5-10 s, uncalibrated).
const IDLE_LIMIT: u32 = 4000;

extern "C" {
    fn _start1();
}

pub fn stage_step() {
    if csr_read!("mhartid") != 0 {
        STAGE.fetch_add(1, SeqCst);
    }
}

fn regs(when: &str) {
    pl!(
        "C1 start {when}: clk_ctrl0={} rst_en0={} pmu_sw_stall={} corestalled_st={}",
        show(m_lw(CLK_CTRL0)),
        show(m_lw(RST_EN0)),
        show(m_lw(PMU_SW_STALL)),
        show(m_lw(CORESTALLED_ST))
    );
}

/// Core 0: start core 1, watch it, end the run. Never returns.
pub fn launch(id: &str) -> ! {
    pl!("lichen-probe-p4 {id} (#608 run 8), core 0 launches core 1 and watches; core 1 runs the tests");
    regs("before");
    // esp_cpu_unstall(1): stall code 0xFF, then wait (bounded) for the stalled bit to drop.
    if let Ok(v) = m_lw(PMU_SW_STALL) {
        let _ = m_sw(PMU_SW_STALL, (v & !0x00FF_0000) | 0x00FF_0000);
    }
    let mut n = 0u32;
    while m_lw(CORESTALLED_ST).map_or(false, |v| v & 2 != 0) && n < 1_000_000 {
        n += 1;
    }
    check("C1.unstall", Some(n < 1_000_000), format_args!("core 1 stalled bit cleared after {n} polls"));
    // cpu_utility_ll_enable_clock_and_reset_app_cpu(): clock on, then out of reset.
    if let Ok(v) = m_lw(CLK_CTRL0) {
        if v & (1 << 4) == 0 {
            let _ = m_sw(CLK_CTRL0, v | (1 << 4));
        }
    }
    if let Ok(v) = m_lw(RST_EN0) {
        if v & (1 << 8) != 0 {
            let _ = m_sw(RST_EN0, v & !(1 << 8));
        }
    }
    regs("after");
    pl!(
        "C1 boot address {:#010x} handed to the ROM; core 0 prints nothing more until core 1 is done or silent",
        _start1 as *const () as usize
    );
    hw::drain();
    // SAFETY: ROM function `void ets_set_appcpu_boot_addr(uint32_t)` at its esp32p4.rom.ld address.
    let set_boot: extern "C" fn(u32) = unsafe { core::mem::transmute(ETS_SET_APPCPU_BOOT_ADDR) };
    set_boot(_start1 as *const () as usize as u32);

    let (mut last, mut idle) = (0u32, 0u32);
    let stage = loop {
        hw::spin(10_000);
        let s = STAGE.load(SeqCst);
        if s == DONE {
            break None;
        }
        if s != last {
            (last, idle) = (s, 0);
        } else {
            idle += 1;
            if idle > IDLE_LIMIT {
                break Some(s);
            }
        }
    };
    match stage {
        None => pl!("END ok"),
        Some(0) => {
            pl!("CHECK C1.watchdog FAIL core 1 never reported (stage 0): it did not start or cannot reach L2MEM");
            pl!("END core1-silent");
        }
        Some(s) => {
            pl!("CHECK C1.watchdog INFO core 1 silent at stage {s} (the last C1 BEGIN above); core 0 still runs");
            pl!("END core1-silent");
        }
    }
    hw::drain();
    loop {
        // SAFETY: park; the host hard-resets the badge after END.
        unsafe { core::arch::asm!("wfi", options(nomem, nostack)) };
    }
}

#[no_mangle]
extern "C" fn p1main() -> ! {
    STAGE.store(1, SeqCst);
    pl!(
        "C1 up: mhartid={} mstatus={:#010x} mtvec={:#010x}",
        csr_read!("mhartid"),
        csr_read!("mstatus"),
        csr_read!("mtvec")
    );
    crate::b5_dump();
    crate::setup_pmp();
    crate::b1_user_mode();
    crate::b3_interrupt_in_u();
    // p4os reports that a U access to the CPU-subsystem window hangs core 1; on core 0 it faults (#608 run 6/7).
    begin("B4.grant4k");
    pmp_set(E_T2, napot(0x2080_0000, 0x1000), NAPOT | R | W);
    let t = user(uaddr(crate::u_load), CLIC_THRESH, 0, false);
    check(
        "B4.grant4k",
        None,
        format_args!("U read of own CLIC threshold {CLIC_THRESH:#x} with a 4 KB NAPOT RW grant: {t}"),
    );
    pmp_off(E_T2);
    begin("B4.mtime-u");
    pmp_set(E_T1, napot(CLINT, 0x1000_0000), NAPOT | R | W);
    let t = user(uaddr(crate::u_load), MTIME_LO, 0, false);
    check(
        "B4.mtime-u",
        None,
        format_args!("U read of mtime lo {MTIME_LO:#x} with the bootloader's entry-0 grant: {t} a0={:#x}", t.a0),
    );
    pmp_off(E_T1);
    pl!("C1 done");
    hw::drain();
    STAGE.store(DONE, SeqCst);
    loop {
        // SAFETY: park; core 0 ends the run.
        unsafe { core::arch::asm!("wfi", options(nomem, nostack)) };
    }
}
