//! #620 PSRAM probe (RAM-only, load-ram). The stage is baked in at build time (LICHEN_PSRAM_STAGE, build.sh):
//! - dump: print state only (eFuse LDO fields, the LDO, clocks, MPLL, PSRAM controller). Writes nothing.
//! - all: dump, then the full bring-up and pattern tests (added step by step in #620).
//!
//! Every line is `psram: ...`; the image ends with `END ok` (load-ram stops there) and parks.
#![no_std]
#![no_main]

use core::arch::global_asm;
use core::fmt::Write;
use lichen_platform_esp32p4::{clock, mmio::read32, psram, uart};

global_asm!(include_str!("boot.S"));

struct Con;
impl Write for Con {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for b in s.bytes() {
            if b == b'\n' {
                put(b'\r');
            }
            put(b);
        }
        Ok(())
    }
}
fn put(b: u8) {
    while !uart::tx_ready() {}
    uart::put_byte(b);
}
macro_rules! pl {
    ($($a:tt)*) => {{ let _ = { use core::fmt::Write as _; writeln!($crate::Con, $($a)*) }; }};
}

mod tests;
mod variants;

const STAGE: &str = env!("LICHEN_PSRAM_STAGE");

const fn parse_override(s: &str) -> Option<u32> {
    let b = s.as_bytes();
    if b.is_empty() || b[0] == b'-' {
        return None;
    }
    let (mut i, mut v) = (0, 0u32);
    while i < b.len() {
        v = v * 10 + (b[i] - b'0') as u32;
        i += 1;
    }
    Some(v)
}
const GROUP: u32 = match parse_override(env!("LICHEN_PSRAM_GROUP")) {
    Some(g) => g,
    None => 1,
};
const OVERRIDE: psram::TuneOverride = psram::TuneOverride {
    phase: parse_override(env!("LICHEN_PSRAM_PHASE")),
    data_dl: parse_override(env!("LICHEN_PSRAM_DATA_DL")),
    dqs_dl: parse_override(env!("LICHEN_PSRAM_DQS_DL")),
};

#[panic_handler]
fn panic(i: &core::panic::PanicInfo) -> ! {
    pl!("psram: PANIC {i}");
    park()
}
fn park() -> ! {
    uart::drain();
    loop {
        core::hint::spin_loop();
    }
}

/// Any trap: report the cause once and park. The probe takes none on purpose, so one is a result.
#[no_mangle]
extern "C" fn ptrap() -> ! {
    let (c, e, t): (usize, usize, usize);
    // SAFETY: reads of M-mode trap CSRs.
    unsafe {
        core::arch::asm!("csrr {}, mcause", "csrr {}, mepc", "csrr {}, mtval", out(reg) c, out(reg) e, out(reg) t)
    };
    pl!("psram: TRAP mcause={c:#x} mepc={e:#x} mtval={t:#x}");
    park()
}

#[no_mangle]
extern "C" fn pmain() -> ! {
    pl!("psram: probe stage={STAGE}");
    dump();
    // "cpu" = clock only; "cpu-<stage>" = clock first, then <stage>
    let mut stage = STAGE;
    let mut cpu_mhz = 40;
    if let Some(rest) = STAGE.strip_prefix("cpu") {
        let target = 360;
        stage = rest.strip_prefix('-').unwrap_or("");
        let mut log = |a: core::fmt::Arguments| pl!("psram: {a}");
        // SAFETY: the one caller, boot hart, RAM-only image (see clock::cpu_to_360).
        match unsafe { clock::cpu_to_360(&mut log) } {
            Ok((_, Some(a))) => {
                cpu_mhz = a;
                pl!("PERF cpu.clock mhz={cpu_mhz}");
                pl!(
                    "CHECK 645.cpu{target} {} measured {cpu_mhz} MHz ({target} expected)",
                    if cpu_mhz * 100 >= target * 97 && cpu_mhz * 100 <= target * 103 { "PASS" } else { "FAIL" }
                );
            }
            Ok(_) => pl!("psram: clock set, timer did not count: no measurement"),
            Err(e) => pl!("CHECK 645.cpu{target} FAIL {e:?}"),
        }
    }
    let upto = match stage {
        "ldo" => Some(psram::Upto::Ldo),
        "mpll" => Some(psram::Upto::Mpll),
        "chip" => Some(psram::Upto::Chip),
        "tune" | "all" | "eye" | "diag" | "perf" | "regs" | "variants" | "isolate" => Some(psram::Upto::Tuned),
        _ => None,
    };
    if let Some(upto) = upto {
        let mut log = |a: core::fmt::Arguments| pl!("psram: {a}");
        // SAFETY: the one caller, on the boot hart, in a RAM-only image that owns the machine (load-ram).
        match unsafe { psram::init(upto, OVERRIDE, &mut log) } {
            Ok(i) => {
                pl!("psram: init ok up to {upto:?}: mpll {} MHz, {} MB", i.mpll_mhz, i.size_bytes >> 20);
                if stage == "isolate" {
                    variants::isolate();
                }
                if stage == "variants" {
                    if let Some(t) = i.tuned {
                        variants::run_group(GROUP, t);
                    }
                }
                if stage == "regs" {
                    regs_dump();
                }
                if stage == "perf" {
                    tests::perf(cpu_mhz);
                }
                if stage == "diag" {
                    tests::diag();
                }
                if stage == "eye" {
                    clock_report();
                    tests::eye();
                }
                if stage == "all" {
                    clock_report();
                    let fails = tests::run(cpu_mhz);
                    pl!(
                        "psram: RESULT {} ({fails} failed tests) at {} MHz",
                        if fails == 0 { "PASS" } else { "FAIL" },
                        psram::PSRAM_MHZ
                    );
                }
            }
            Err(e) => pl!("psram: init FAILED {e:?}"),
        }
    }
    pl!("END ok");
    park()
}

/// Every non-zero register word of the PSRAM controller (cache side SPIMEM2, FIFO side SPIMEM3) and the MSPI pin block,
/// to compare with the same addresses read on v2 (`mem_int_raw` at SPIMEM2 + 0xC8 holds TX underflow / RX overflow flags).
fn regs_dump() {
    for (name, base, len) in
        [("spimem2", 0x5008_E000usize, 0x400usize), ("spimem3", 0x5008_F000, 0x200), ("mspi_iomux", 0x500E_1200, 0x70)]
    {
        let mut a = 0;
        while a < len {
            let v = rd(base + a);
            if v != 0 {
                pl!("REGS {name}+{a:#05x} = {v:#010x}");
            }
            a += 4;
        }
    }
    pl!("REGS mspi2 int_raw {:#010x} (bit 26 rx overflow, bit 27 tx underflow)", rd(0x5008_E000 + 0xC8));
}

/// The clocks the pattern tests ran at, read back right before them.
fn clock_report() {
    pl!(
        "psram: clocks: bus clock words cache side {:#010x} fifo side {:#010x} (480 / div), peri_clk_ctrl00 {:#010x}, root0 {:#010x} root1 {:#010x}",
        rd(psram::regs::BUS_CLOCK_SPIMEM2),
        rd(psram::regs::BUS_CLOCK_SPIMEM3),
        rd(0x500E_6030),
        rd(0x500E_6004),
        rd(0x500E_6008)
    );
}

fn rd(a: usize) -> u32 {
    // SAFETY: the callers pass addresses of readable peripheral registers of the P4 (see each table).
    unsafe { read32(a) }
}

/// Read-only state dump: nothing here writes a register.
fn dump() {
    // eFuse: the two read registers only (psram::ldo::read_ldo_efuse). Print the extracted fields, never the raw words.
    // SAFETY: read-only, see read_ldo_efuse.
    let e = unsafe { psram::ldo::read_ldo_efuse() };
    let (dref, mul) = psram::ldo::ldo_unit1_dref_mul(psram::ldo::LDO_PSRAM_MV, &e);
    pl!(
        "psram: efuse blk_version={} vo2_dref={} vo2_mul={} -> {} mV needs dref={dref} mul={mul} ({})",
        e.blk_version,
        e.vo2_dref,
        e.vo2_mul,
        psram::ldo::LDO_PSRAM_MV,
        if e.blk_version >= 1 && e.vo2_dref != 0 && e.vo2_mul != 0 {
            "from the eFuse calibration"
        } else {
            "IDF fallback"
        }
    );
    let (l, a) = psram::ldo::ldo_psram_state();
    let f = psram::ldo::ldo_fields(l, a);
    pl!(
        "psram: ldo VO2 now: xpd={} sw_owner={} tieh_sel={} tieh={} dref={} mul={} cur_lim={} vdet={}",
        f[0],
        f[1],
        f[2],
        f[3],
        f[4],
        f[5],
        f[6],
        f[7]
    );
    // Clocks. HP_SYS_CLKRST 0x500E6000: root_clk_ctrl0..3 at +4, soc_clk_ctrl0..3 at +0x14, ref_clk_ctrl at +0x24,
    // peri_clk_ctrl00 at +0x30, ana_pll_ctrl0 at +0xBC.
    const CLKRST: usize = 0x500E_6000;
    for (n, off) in [
        ("root0", 0x04),
        ("root1", 0x08),
        ("root2", 0x0C),
        ("root3", 0x10),
        ("soc0", 0x14),
        ("soc1", 0x18),
        ("soc2", 0x1C),
        ("soc3", 0x20),
        ("peri00", 0x30),
        ("peri01", 0x34),
        ("anapll0", 0xBC),
    ] {
        pl!("psram: clkrst {n} {:#010x}", rd(CLKRST + off));
    }
    let (lpperi, clk160) = psram::pll::i2c_master_state();
    pl!("psram: analog i2c master: lpperi clk_en {lpperi:#010x} (bit 27 i2cmst), clk160m {clk160:#x}");
    pl!("psram: pmu_rf_pwc {:#010x} (bit 24 MPLL XPD)", rd(0x5011_515C));
    pl!("psram: lp hp_clk_ctrl {:#010x} (bit 28 MPLL clock)", rd(0x5011_1040));
    // PSRAM controller (MSPI2): a few registers, reads only.
    const MSPI2: usize = 0x5008_E000;
    for (n, off) in [
        ("fctrl", 0x3C),
        ("sctrl", 0x40),
        ("sram_cmd", 0x44),
        ("drd_cmd", 0x48),
        ("dwr_cmd", 0x4C),
        ("sram_clk", 0x50),
        ("smem_ddr", 0xD8),
    ] {
        pl!("psram: mspi2 {n} {:#010x}", rd(MSPI2 + off));
    }
    // Cache controller, as the ROM left it.
    pl!("psram: l2 cachesize {:#010x}", lichen_platform_esp32p4::cache::l2_cachesize_reg());
}
