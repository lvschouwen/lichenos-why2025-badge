//! #620 init variants, one knob at a time against the uncached address-in-address test (the only one that fails):
//! `variants` stage, groups 1 to 3 (`LICHEN_PSRAM_GROUP`). Each variant applies one change on top of the tuned init, runs the
//! test, and puts the knob back. Output: `VARIANT <name>: <register change> -> <bad words> ...` lines.
use core::ptr::{read_volatile, write_volatile};
use lichen_platform_esp32p4::mmio::{modify32, read32, write32};
use lichen_platform_esp32p4::psram::map::{self, CACHED_BASE, PAGE_BYTES, UNCACHED_OFFSET};
use lichen_platform_esp32p4::psram::{mspi, patterns, regs, tune};

const UNC: usize = CACHED_BASE + UNCACHED_OFFSET;
const OFF: usize = 0x50_0000;
const LEN: usize = 6 << 20;
const SPIMEM2: usize = 0x5008_E000;
const INT_RAW: usize = SPIMEM2 + 0xC8;
const INT_ENA: usize = SPIMEM2 + 0xC0;
const INT_CLR: usize = SPIMEM2 + 0xC4;
/// ECC_ERR, PMS_REJECT, AXI_RADDR_ERR, AXI_WR_FLASH_ERR, AXI_WADDR_ERR, RX_TRANS_OVF, TX_TRANS_UDF (PSRAM_CTRLR_LL_EVENT_MASK)
const EVENT_MASK: u32 = 0x0C00_03E0;

/// Plain address-in-address, uncached, 6 MB at 0x500000. Returns (bad words, first offsets, bit-8 count, other-bit count).
fn aia() -> (u32, [u32; 6], u32, u32) {
    let (mut bad, mut first, mut bit8, mut other) = (0u32, [0u32; 6], 0u32, 0u32);
    let mut a = OFF;
    while a < OFF + LEN {
        // SAFETY: inside the 32 MB window mapped by `map_identity`.
        unsafe { write_volatile((UNC + a) as *mut u32, patterns::addr_word(a as u32, false)) };
        a += 4;
    }
    let mut a = OFF;
    while a < OFF + LEN {
        // SAFETY: as above.
        let g = unsafe { read_volatile((UNC + a) as *const u32) };
        let e = a as u32;
        if g != e {
            if (bad as usize) < first.len() {
                first[bad as usize] = a as u32;
            }
            bad += 1;
            if (g ^ e) == 0x100 {
                bit8 += 1;
            } else {
                other += 1;
            }
        }
        a += 4;
    }
    (bad, first, bit8, other)
}

fn run(name: &str, change: core::fmt::Arguments) {
    // SAFETY: ROM cache maintenance on the mapped window; interrupt flag registers.
    let (r, raw_before) = unsafe {
        let raw = read32(INT_RAW);
        write32(INT_CLR, 0xFFFF_FFFF);
        (map::cache_invalidate(CACHED_BASE, 32 << 20), raw)
    };
    let (bad, first, bit8, other) = aia();
    // SAFETY: read of the controller's raw interrupt flags.
    let raw = unsafe { read32(INT_RAW) };
    pl!("VARIANT {name}: {change} -> {bad} bad of 1572864 words (bit 8 only: {bit8}, other bits: {other}); int_raw before {raw_before:#x} after {raw:#x}; invalidate {r}");
    if bad != 0 {
        pl!(
            "VARIANT {name}:   first offsets {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}",
            first[0],
            first[1],
            first[2],
            first[3],
            first[4],
            first[5]
        );
    }
    lichen_platform_esp32p4::uart::drain();
}

fn set_tune(phase: u32, data: u32, dqs: u32) {
    // SAFETY: MSPI IOMUX tuning registers; probe only.
    unsafe {
        regs::set_dqs_phase(phase);
        regs::set_delaylines(data, dqs);
    }
}
/// Write the mode register MR4 write-latency field (bits 7:5) over the FIFO path (variable dummy off, as at init).
fn set_wlat(wl: u8) {
    // SAFETY: FIFO-path mode-register transactions on the initialised controller.
    unsafe {
        regs::apply(&[regs::VAR_DUMMY_FIFO_OFF]);
        let mut r = [0u32; 1];
        mspi::xact(mspi::REG_READ, 4, mspi::RD_REG_DUMMY_BITLEN, None, Some((&mut r, 16)));
        let (m4, m5) = (r[0] as u8, (r[0] >> 8) as u8);
        let new = (m4 & 0x1F) | (wl << 5);
        mspi::xact(mspi::REG_WRITE, 4, 0, Some((&[new as u32 | ((m5 as u32) << 8)], 16)), None);
        mspi::xact(mspi::REG_READ, 4, mspi::RD_REG_DUMMY_BITLEN, None, Some((&mut r, 8)));
        pl!("VARIANT note: MR4 now {:#04x}", r[0] as u8);
        regs::apply(&regs::VAR_DUMMY_ON);
    }
}
/// AXI-side write dummy field: SPIMEM2 + 0x40 bits 27:22 = dummy bits - 1 (IDF: 2 * (5 - 1) = 8 bits).
fn set_wdummy(bits: u32) {
    // SAFETY: PSRAM controller cache-side command register.
    unsafe { modify32(SPIMEM2 + 0x40, 0x0FC0_0000, (bits - 1) << 22) };
}

pub fn run_group(group: u32, base: tune::Tuned) {
    // SAFETY: ROM cache maintenance; mapped window.
    unsafe {
        map::cache_invalidate_all_l2();
        map::map_identity((32 << 20) / PAGE_BYTES);
    }
    let (bp, bd, bq) = (tune::PHASES[base.phase_id], base.data_dl, base.dqs_dl);
    pl!("VARIANT base point: phase {bp} data {bd} dqs {bq} (delay line id {})", base.delayline_id);
    run("base", format_args!("no change"));
    match group {
        1 => {
            // (1) L2 as v2: 256 KB, 8 ways, 128 B lines (Cache_Set_L2_Cache_Mode(10, 8, 4) + invalidate all)
            // SAFETY: nothing is cached through L2 at this point (all accesses so far were uncached or invalidated).
            unsafe {
                map::cache_set_l2_mode(10, 8, 4);
                map::cache_invalidate_all_l2();
            }
            run(
                "l2-v2",
                format_args!("Cache_Set_L2_Cache_Mode(256K, 8 ways, 128 B); L2 block reg now {:#x}", unsafe {
                    read32(0x3FF1_027C)
                }),
            );
            // (2) MSPI error interrupts enabled (flags, no CPU interrupt taken: mstatus.MIE is off)
            // SAFETY: controller interrupt enable register.
            unsafe { write32(INT_ENA, EVENT_MASK) };
            run("irq", format_args!("mem_int_ena {EVENT_MASK:#x}"));
            // (3) tuning points around the earlier pick, id 17 = (data 2, dqs 0)
            for (id, name) in [(16usize, "tune-id16"), (17, "tune-id17"), (18, "tune-id18")] {
                let (d, q) = tune::delayline(id);
                set_tune(bp, d, q);
                run(name, format_args!("phase {bp} data {d} dqs {q}"));
            }
            set_tune(bp, bd, bq);
        }
        2 => {
            // (4) write-side timing: AXI write dummy bits
            for bits in [6u32, 10, 12] {
                set_wdummy(bits);
                run("wdummy", format_args!("SPIMEM2+0x40 write dummy bits = {bits} (IDF 8)"));
            }
            set_wdummy(8);
            // output drive strength of all MSPI pins (IDF 2)
            for drv in [1u32, 3] {
                // SAFETY: MSPI pin registers.
                unsafe { regs::pin_drv_set(drv) };
                run("drv", format_args!("pin drive = {drv} (IDF 2)"));
            }
            // SAFETY: as above.
            unsafe { regs::pin_drv_set(2) };
            // DQS phase neighbours, same delay line
            for ph in 1..4u32 {
                set_tune(ph, bd, bq);
                run("phase", format_args!("DQS phase {ph} (pick {bp}), data {bd} dqs {bq}"));
            }
            set_tune(bp, bd, bq);
        }
        _ => {
            // write latency code in MR4 with the matching AXI write dummy (one more cycle = 2 more dummy bits per step)
            for (wl, bits) in [(3u8, 10u32), (1, 6)] {
                set_wlat(wl);
                set_wdummy(bits);
                run("wlat", format_args!("MR4 write latency field {wl} (IDF 2) with AXI write dummy {bits} bits"));
            }
            set_wlat(2);
            set_wdummy(8);
            // DQS delay line variations with the data line at 0
            for q in [1u32, 3, 4] {
                set_tune(bp, 0, q);
                run("dqs", format_args!("data 0 dqs {q}"));
            }
            set_tune(bp, bd, bq);
            run("base-again", format_args!("everything put back"));
        }
    }
}

// ---- isolate: does the flip follow the data, the neighbours, the address or the write rate? -----------------------------
const I_OFF: usize = 0x54_0000;
const I_LEN: usize = 0x8_0000; // 0x540000..0x5C0000 covers 0x55xxxx-0x5axxxx

#[derive(Clone, Copy)]
enum Store {
    Word,
    Half,
    Byte,
}

fn cycles() -> u32 {
    let v: u32;
    // SAFETY: mcycle CSR read.
    unsafe { core::arch::asm!("csrr {}, mcycle", out(reg) v) };
    v
}

fn iso_case(name: &str, f: &dyn Fn(usize) -> u32, how: Store, gap_cycles: u32) {
    // SAFETY: ROM cache maintenance on the mapped window.
    unsafe { map::cache_invalidate(CACHED_BASE, 32 << 20) };
    let mut a = I_OFF;
    while a < I_OFF + I_LEN {
        let v = f(a);
        let p = UNC + a;
        // SAFETY: inside the mapped window, naturally aligned.
        unsafe {
            match how {
                Store::Word => write_volatile(p as *mut u32, v),
                Store::Half => {
                    write_volatile(p as *mut u16, v as u16);
                    write_volatile((p + 2) as *mut u16, (v >> 16) as u16);
                }
                Store::Byte => {
                    for i in 0..4 {
                        write_volatile((p + i) as *mut u8, (v >> (8 * i)) as u8);
                    }
                }
            }
            core::arch::asm!("fence");
        }
        if gap_cycles != 0 {
            let t0 = cycles();
            while cycles().wrapping_sub(t0) < gap_cycles {}
        }
        a += 4;
    }
    let (mut bad, mut bit8, mut other, mut in_class, mut first) = (0u32, 0u32, 0u32, 0u32, [0u32; 4]);
    let mut a = I_OFF;
    while a < I_OFF + I_LEN {
        // SAFETY: as above.
        let g = unsafe { read_volatile((UNC + a) as *const u32) };
        let e = f(a);
        if g != e {
            if (bad as usize) < first.len() {
                first[bad as usize] = a as u32;
            }
            bad += 1;
            if g ^ e == 0x100 {
                bit8 += 1;
            } else {
                other += 1;
            }
            if a & 0x7FF == 0x400 {
                in_class += 1;
            }
        }
        a += 4;
    }
    pl!(
        "ISOLATE {name}: {bad} bad of {} words (bit 8 only {bit8}, other {other}, at offset 0x400 mod 0x800: {in_class}); first {:#x} {:#x} {:#x} {:#x}",
        I_LEN / 4,
        first[0],
        first[1],
        first[2],
        first[3]
    );
    lichen_platform_esp32p4::uart::drain();
}

pub fn isolate() {
    // SAFETY: ROM cache maintenance; mapped window.
    unsafe {
        map::cache_invalidate_all_l2();
        map::map_identity((32 << 20) / PAGE_BYTES);
    }
    let addr = |a: usize| a as u32;
    iso_case("address, sw, back to back", &addr, Store::Word, 0);
    iso_case("const 0x00000400", &|_| 0x400, Store::Word, 0);
    iso_case("const 0x00000000", &|_| 0, Store::Word, 0);
    iso_case("const 0x00000600", &|_| 0x600, Store::Word, 0);
    iso_case("const 0x00000200 (bit 9)", &|_| 0x200, Store::Word, 0);
    iso_case("const 0xFFFFFFFF (control)", &|_| 0xFFFF_FFFF, Store::Word, 0);
    // neighbours: only the words at page offset 0x400 differ from their surroundings
    iso_case(
        "0x400 at offset 0x400 mod 0x800, 0x3FC elsewhere",
        &|a| if a & 0x7FF == 0x400 { 0x400 } else { 0x3FC },
        Store::Word,
        0,
    );
    iso_case(
        "0x400 at offset 0x400 mod 0x800, 0 elsewhere",
        &|a| if a & 0x7FF == 0x400 { 0x400 } else { 0 },
        Store::Word,
        0,
    );
    iso_case(
        "0 at offset 0x400 mod 0x800, 0x400 elsewhere",
        &|a| if a & 0x7FF == 0x400 { 0 } else { 0x400 },
        Store::Word,
        0,
    );
    // write rate: 1 us (360 cycles) between stores, and 10 us
    iso_case("address, sw, 1 us gap", &addr, Store::Word, 360);
    iso_case("address, sw, 10 us gap", &addr, Store::Word, 3600);
    iso_case("const 0x400, sw, 1 us gap", &|_| 0x400, Store::Word, 360);
    // store width
    iso_case("address, sh x2", &addr, Store::Half, 0);
    iso_case("address, sb x4", &addr, Store::Byte, 0);
    iso_case("address, sb x4, 1 us gap", &addr, Store::Byte, 360);
}

/// The uncached address-in-address diagnostic once (for comparing speeds), after the window is mapped by the caller.
pub fn aia_base() {
    run("base", format_args!("no change, uncached single-word stores (diagnostic only)"));
}
