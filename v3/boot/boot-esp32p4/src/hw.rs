//! The chip glue: UART0 console, the watchdogs the ROM arms, ROM flash read, the memory map. Register numbers are
//! from the v1.0 headers (hw_ver1). Nothing here touches the eFuse controller, the clocks, the MMU/cache or PSRAM.
use boot_core::console::Console;
use boot_core::flash::{FlashError, FlashRead};
use boot_core::mem::{MemoryMap, Range};

#[inline(always)]
unsafe fn rd(addr: usize) -> u32 {
    // SAFETY: caller passes a 4-byte aligned M-mode readable register.
    unsafe { core::ptr::read_volatile(addr as *const u32) }
}
#[inline(always)]
unsafe fn wr(addr: usize, v: u32) {
    // SAFETY: caller passes a 4-byte aligned M-mode writable register.
    unsafe { core::ptr::write_volatile(addr as *mut u32, v) }
}

// ---- console: UART0, as the ROM left it (115200 8N1) ----
const UART0: usize = 0x500C_A000;
const UART_STATUS: usize = UART0 + 0x1C; // TXFIFO_CNT in [23:16]

pub struct Uart;

impl Uart {
    /// Wait until the FIFO is empty (the last bytes are on their way out).
    pub fn drain() {
        // SAFETY: UART0 status register, a read.
        while (unsafe { rd(UART_STATUS) } >> 16) & 0xFF != 0 {}
        for i in 0..20_000u32 {
            core::hint::black_box(i);
        }
    }
}

impl Console for Uart {
    fn put(&mut self, b: u8) {
        // SAFETY: UART0 status read, then a TX FIFO write; the ROM configured the UART.
        unsafe {
            while (rd(UART_STATUS) >> 16) & 0xFF >= 120 {}
            wr(UART0, b as u32);
        }
    }
}

// ---- watchdogs ----
const WDT_KEY: u32 = 0x50D8_3AA1;
const SWD_KEY: u32 = 0x50D8_3AA1; // LP_WDT_SWD_WKEY_VALUE, esp_hal_wdt/esp32p4 lpwdt_ll.h (not the C3 key)
const TIMG: [usize; 2] = [0x500C_2000, 0x500C_3000];
const TIMG_WDTCONFIG0: usize = 0x48;
const TIMG_WDTWPROTECT: usize = 0x64;
const LP_WDT: usize = 0x5011_6000;
const LP_CONFIG0: usize = LP_WDT;
const LP_WPROTECT: usize = LP_WDT + 0x18;
const LP_SWD_CONFIG: usize = LP_WDT + 0x1C;
const LP_SWD_WPROTECT: usize = LP_WDT + 0x20;
const WDT_EN: u32 = 1 << 31;
const TIMG_FLASHBOOT: u32 = 1 << 14;
const LP_FLASHBOOT: u32 = 1 << 12;
const SWD_AUTO_FEED: u32 = 1 << 18;
const SWD_FEED: u32 = 1 << 31;

/// Config registers as the ROM left them: TIMG0, TIMG1, LP RWDT, LP super watchdog.
pub fn wdt_state() -> [u32; 4] {
    // SAFETY: four watchdog config registers, reads.
    unsafe { [rd(TIMG[0] + TIMG_WDTCONFIG0), rd(TIMG[1] + TIMG_WDTCONFIG0), rd(LP_CONFIG0), rd(LP_SWD_CONFIG)] }
}

/// Switch off MWDT0, MWDT1 and the RWDT, and put the super watchdog on auto-feed. Only these registers are written.
pub fn wdt_disable() {
    // SAFETY: documented unlock/modify/lock sequences of the watchdog registers; the stage is the only code running.
    unsafe {
        for t in TIMG {
            wr(t + TIMG_WDTWPROTECT, WDT_KEY);
            let c = rd(t + TIMG_WDTCONFIG0);
            wr(t + TIMG_WDTCONFIG0, c & !(WDT_EN | TIMG_FLASHBOOT));
            wr(t + TIMG_WDTWPROTECT, 0);
        }
        wr(LP_WPROTECT, WDT_KEY);
        let c = rd(LP_CONFIG0);
        wr(LP_CONFIG0, c & !(WDT_EN | LP_FLASHBOOT));
        wr(LP_WPROTECT, 0);
        wr(LP_SWD_WPROTECT, SWD_KEY);
        let c = rd(LP_SWD_CONFIG);
        wr(LP_SWD_CONFIG, c | SWD_AUTO_FEED | SWD_FEED);
        wr(LP_SWD_WPROTECT, 0);
    }
}

// ---- flash: the ROM's esp_rom_spiflash_read (esp32p4.rom.eco0_4.ld, same address in esp32p4.rom.ld) ----
// Only the read entry and the parameter setter. Not unlock/clear_bp (they write the status register) and not
// spi_flash_attach (it queries eFuse pins); the ROM download mode that loaded us has already attached the flash.
// The ROM's bounds check uses its chip size, which download mode leaves at 2 MB (bench 2026-11-27: reads at or past
// 0x200000 return 1), so `config_param` sets it the way IDF's bootloader does (bootloader_flash_config_*.c). It only
// stores the chip parameters in the ROM's RAM state: no SPI transaction, no eFuse.
const ROM_SPIFLASH_READ: usize = 0x4FC0_0158;
const ROM_SPIFLASH_CONFIG_PARAM: usize = 0x4FC0_0168;
type RomRead = extern "C" fn(src: u32, dest: *mut u32, len: i32) -> i32;
type RomConfigParam = extern "C" fn(id: u32, chip: u32, block: u32, sector: u32, page: u32, mask: u32) -> i32;
/// The badge's flash (partition table: storage ends at 0x1000000).
const FLASH_SIZE: u32 = 16 * 1024 * 1024;

/// Tell the ROM read the real flash size. Returns the ROM's result code (0 = ok).
pub fn flash_set_size() -> i32 {
    // SAFETY: a fixed ROM entry point of this chip with the C signature above; the device id 0 is only used by the
    // ROM's write paths, which this stage never calls.
    let f: RomConfigParam = unsafe { core::mem::transmute(ROM_SPIFLASH_CONFIG_PARAM) };
    f(0, FLASH_SIZE, 0x10000, 0x1000, 0x100, 0xFFFF)
}

/// Bench aid: the `trace` feature prints every ROM read call that fails: address, length, return code.
const TRACE: bool = cfg!(feature = "trace");

fn trace(tag: u8, a: u32, b: u32) {
    let mut c = Uart;
    c.put(tag);
    c.put(b' ');
    c.hex32(a);
    c.put(b' ');
    c.hex32(b);
    c.put(b'\n');
}

pub struct RomFlash;

impl FlashRead for RomFlash {
    fn read(&mut self, offset: u32, buf: &mut [u8]) -> Result<(), FlashError> {
        // The ROM wants a word-aligned source and length and a word-aligned destination: bounce through a buffer.
        let mut bounce = [0u32; 64];
        let mut off = offset;
        let mut done = 0;
        while done < buf.len() {
            let a = off & !3;
            let skip = (off - a) as usize;
            let want = (buf.len() - done).min(bounce.len() * 4 - skip);
            let words = (skip + want).div_ceil(4);
            // SAFETY: a fixed ROM entry point of this chip with the C signature above; `bounce` holds `words` words.
            let f: RomRead = unsafe { core::mem::transmute(ROM_SPIFLASH_READ) };
            let rc = f(a, bounce.as_mut_ptr(), (words * 4) as i32);
            if TRACE && rc != 0 {
                trace(b'!', a, words as u32 * 4);
                trace(b'=', rc as u32, 0);
            }
            if rc != 0 {
                return Err(FlashError);
            }
            for i in 0..want {
                buf[done + i] = (bounce[(skip + i) / 4] >> (((skip + i) % 4) * 8)) as u8;
            }
            done += want;
            off += want as u32;
        }
        Ok(())
    }
}

// ---- memory map ----
/// Loaded bytes of a stage-booted kernel: 0x4FF10000 up to the stage window (kernel/link-esp32p4.ld).
const RAM: [Range; 1] = [Range::new(0x4FF1_0000, 0x4FF2_8ED0)];
/// The window the ROM loads the stage into.
const STAGE_WINDOW: [Range; 1] = [Range::new(0x4FF2_8ED0, 0x4FF3_ABD0)];
/// Flash-mapped address space (IROM/DROM).
const MAPPED: [Range; 1] = [Range::new(0x4000_0000, 0x4C00_0000)];

pub const MEMORY_MAP: MemoryMap<'static> = MemoryMap { ram: &RAM, forbidden: &STAGE_WINDOW, mapped: &MAPPED };
