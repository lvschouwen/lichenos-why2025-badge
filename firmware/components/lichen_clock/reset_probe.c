// The reset-survival probe (#330 spike, CONFIG_LICHEN_RESET_PROBE): #326's LP RAM clock record read back invalid after a
// CH340 EN reset. One marker in each store that could carry the record without a flash write, read at every boot (with
// the reset reason) before it is rewritten, so every reset is a test:
// - rtc: RTC_NOINIT_ATTR LP RAM, the same section as #326's record (cpu_start clears .rtc.bss, never .rtc_noinit; the
//   RTCRAM heap starts after .rtc.force_slow);
// - lp_store: LP_SYSTEM_REG_LP_STORE12..14, which neither IDF 6.1 nor the ROM use (rom/rtc.h maps 0..11; LP_STORE15 is
//   the PSRAM speed carrier, #121, and is never touched here);
// - psram: EXT_RAM_NOINIT_ATTR (CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY, selected by the Kconfig switch): the PSRAM
//   heap and IDF's boot memtest skip that section. The word sits alone in one cache line and every write is written back
//   (esp_cache_msync): a reset drops the cache, and with it a write that never reached the chip.
// Nothing is written to flash and nothing goes on air. Remove this file and the switch once #326 has picked its store.
#include "reset_probe.h"

#include "reset_probe_logic.h"

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "fw_console.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

#include <string.h>

#define PSRAM_LINE 128 // >= the P4's L1 and L2 data cache line: the probe owns its line

enum { ST_RTC, ST_LP, ST_PSRAM };

typedef struct {
    reset_probe_word_t w;
    uint8_t            pad[PSRAM_LINE - sizeof(reset_probe_word_t)];
} psram_line_t;

static RTC_NOINIT_ATTR volatile reset_probe_word_t s_rtc;
static EXT_RAM_NOINIT_ATTR psram_line_t s_psram __attribute__((aligned(PSRAM_LINE)));

static reset_probe_word_t s_read[RESET_PROBE_STORES]; // the raw words the boot found
static bool               s_ok[RESET_PROBE_STORES];
static char               s_line[128];
static bool               s_booted;

static char const TAG[] = "resetprobe";

static char const *const REASON[] = {
    [ESP_RST_UNKNOWN] = "UNKNOWN",     [ESP_RST_POWERON] = "POWERON",   [ESP_RST_EXT] = "EXT",
    [ESP_RST_SW] = "SW",               [ESP_RST_PANIC] = "PANIC",       [ESP_RST_INT_WDT] = "INT_WDT",
    [ESP_RST_TASK_WDT] = "TASK_WDT",   [ESP_RST_WDT] = "WDT",           [ESP_RST_DEEPSLEEP] = "DEEPSLEEP",
    [ESP_RST_BROWNOUT] = "BROWNOUT",   [ESP_RST_SDIO] = "SDIO",         [ESP_RST_USB] = "USB",
    [ESP_RST_JTAG] = "JTAG",           [ESP_RST_EFUSE] = "EFUSE",       [ESP_RST_PWR_GLITCH] = "PWR_GLITCH",
    [ESP_RST_CPU_LOCKUP] = "CPU_LOCKUP",
};

static char const *reason_text(esp_reset_reason_t r) {
    unsigned i = (unsigned)r;
    return i < sizeof(REASON) / sizeof(REASON[0]) && REASON[i] ? REASON[i] : "UNKNOWN";
}

static reset_probe_word_t read_store(int s) {
    switch (s) {
        case ST_RTC: return (reset_probe_word_t){.magic = s_rtc.magic, .count = s_rtc.count, .check = s_rtc.check};
        case ST_LP:
            return (reset_probe_word_t){.magic = REG_READ(LP_SYSTEM_REG_LP_STORE12_REG),
                                        .count = REG_READ(LP_SYSTEM_REG_LP_STORE13_REG),
                                        .check = REG_READ(LP_SYSTEM_REG_LP_STORE14_REG)};
        default: return s_psram.w;
    }
}

// The check word last, after a cleared one: a reset in between leaves a lost marker, never a mixed one
static void write_store(int s, reset_probe_word_t w) {
    switch (s) {
        case ST_RTC:
            s_rtc.check = 0;
            s_rtc.magic = w.magic;
            s_rtc.count = w.count;
            s_rtc.check = w.check;
            break;
        case ST_LP:
            REG_WRITE(LP_SYSTEM_REG_LP_STORE14_REG, 0);
            REG_WRITE(LP_SYSTEM_REG_LP_STORE12_REG, w.magic);
            REG_WRITE(LP_SYSTEM_REG_LP_STORE13_REG, w.count);
            REG_WRITE(LP_SYSTEM_REG_LP_STORE14_REG, w.check);
            break;
        default:
            s_psram.w = w;
            if (esp_cache_msync(&s_psram, sizeof(s_psram), ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) {
                ESP_LOGW(TAG, "psram marker not written back");
            }
            break;
    }
}

void reset_probe_boot(void) {
    if (s_booted) {
        return;
    }
    uint32_t count[RESET_PROBE_STORES];
    for (int s = 0; s < RESET_PROBE_STORES; s++) {
        s_read[s] = read_store(s);
        s_ok[s]   = reset_probe_ok(&s_read[s]);
        count[s]  = s_ok[s] ? s_read[s].count : 0;
    }
    reset_probe_line(s_line, sizeof(s_line), reason_text(esp_reset_reason()), (unsigned)esp_rom_get_reset_reason(0), s_ok,
                     count);
    ESP_LOGI(TAG, "%s", s_line);
    for (int s = 0; s < RESET_PROBE_STORES; s++) {
        write_store(s, reset_probe_next(&s_read[s]));
    }
    s_booted = true;
}

static char const *const STORE_NAME[RESET_PROBE_STORES] = {"rtc", "lp_store", "psram"};

static void cmd_show(void) {
    console_reply("resetprobe at boot: %s", s_line);
    for (int s = 0; s < RESET_PROBE_STORES; s++) {
        reset_probe_word_t const r   = s_read[s];
        reset_probe_word_t const now = read_store(s);
        console_reply("  %-8s boot raw %08x %08x %08x, now %s count %u", STORE_NAME[s], (unsigned)r.magic,
                      (unsigned)r.count, (unsigned)r.check, reset_probe_ok(&now) ? "armed" : "LOST", (unsigned)now.count);
    }
    console_reply("  at rtc %p, lp_store LP_STORE12..14, psram %p", (void *)&s_rtc, (void *)&s_psram);
}

static void cmd_resetprobe(char const *args) {
    if (!s_booted) {
        console_reply("resetprobe: the boot read has not run yet");
        return;
    }
    if (strcmp(args, "arm") == 0) {
        for (int s = 0; s < RESET_PROBE_STORES; s++) {
            write_store(s, reset_probe_make(0));
        }
        console_reply("resetprobe: armed, all three at count 0 (no flash write); reset now, then fw resetprobe show");
        return;
    }
    if (strcmp(args, "show") == 0 || args[0] == '\0') {
        cmd_show();
        return;
    }
    console_reply("usage: resetprobe arm|show");
}

void reset_probe_console_register(void) {
    console_register("resetprobe", "resetprobe arm|show (#330: which store survives which reset; no flash write)",
                     cmd_resetprobe);
}
