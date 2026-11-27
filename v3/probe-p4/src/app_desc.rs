//! Step 3b (#608): what an app image needs for the real v6.1 bootloader to load and start it from ota_1, and for
//! Lichen's card installer to take it.
//! - esp_app_desc_t (esp_app_desc.h, 256 bytes) at the start of segment 0, in a flash-mapped (DROM/IROM) segment like
//!   an IDF app's: the bootloader reads its efuse block revision range and MMU page size there, and maps the segment,
//!   so it never maps a zero-length region. The P4 bootloader wants exactly two flash segments; the second is APP_ROM2. Lichen's installer wants the magic and a non-zero ELF SHA-256, which
//!   esptool writes at image offset 0xb0 (build.sh, --elf-sha256-offset).
//! - Field values as in Lichen's own image (efuse 0-199, MMU page 2^16, flash mode 3).
//!   Built only for LICHEN_PROBE_RUN=app (cfg probe_app), linked by link-app.ld; the load-ram suites keep RAM-only images.

#[cfg(probe_app)]
#[repr(C, align(16))]
pub struct AppDesc([u8; 256]);

#[cfg(probe_app)]
const fn put(mut d: [u8; 256], at: usize, s: &[u8]) -> [u8; 256] {
    let mut i = 0;
    while i < s.len() {
        d[at + i] = s[i];
        i += 1;
    }
    d
}

#[cfg(probe_app)]
const fn desc() -> [u8; 256] {
    let mut d = [0u8; 256];
    d = put(d, 0, &0xABCD_5432u32.to_le_bytes()); // magic_word
    d = put(d, 16, super::PROBE_ID.as_bytes()); // version[32] (an 8-hex id, -dirty at most, then -<#609 suite>)
    if !super::SUITE.is_empty() {
        d = put(d, 16 + super::PROBE_ID.len(), b"-");
        d = put(d, 17 + super::PROBE_ID.len(), super::SUITE.as_bytes());
    }
    d = put(d, 48, b"lichen-probe-p4"); // project_name[32]
    d = put(d, 112, b"none (bare-metal probe)"); // idf_ver[32]
    // app_elf_sha256[32] at 144: esptool fills it
    d = put(d, 176, &0u16.to_le_bytes()); // min_efuse_blk_rev_full
    d = put(d, 178, &199u16.to_le_bytes()); // max_efuse_blk_rev_full
    d[180] = 16; // mmu_page_size, log2: 64 KB
    d[181] = 3; // spi_flash_mode, as Lichen's
    d
}

#[cfg(probe_app)]
#[used]
#[link_section = ".app_desc"]
static APP_DESC: AppDesc = AppDesc(desc());

/// The second flash segment the bootloader wants (link-app.ld); never read.
#[cfg(probe_app)]
#[used]
#[link_section = ".app_rom2"]
static APP_ROM2: [u8; 16] = *b"lichen-probe-p4\0";

/// The ROM's software_reset (esp32p4.rom.ld: 0x4fc00094): a system reset, so the bootloader runs again. The
/// bootloader's 9 s RTC watchdog, which the probe never feeds, is the backstop should it return.
pub fn reset() -> ! {
    // SAFETY: a ROM entry point that takes no arguments and resets the chip.
    let f: extern "C" fn() -> ! = unsafe { core::mem::transmute(0x4fc0_0094usize) };
    f()
}
