#include "psram_boot.h"

#include "psram_dma_cmd.h"
#include "psram_pattern.h"
#include "psram_speed.h"
#include "psram_tuning.h"

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "hal/efuse_hal.h"
#include "hal/mspi_ll.h"
#include "nvs.h"
#include "soc/efuse_struct.h"
#include "soc/iomux_mspi_pin_reg.h"
#include "soc/iomux_mspi_pin_struct.h"
#include "soc/pmu_struct.h"
#include "soc/soc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK_BYTES   (4u * 1024 * 1024)
#define LINE          128
#define RETRY_KEY     "psram_rty"
#define LDO_PSRAM_IDX 3 // PMU ext_ldo index of LDO unit 1 (VO2), the PSRAM channel (CONFIG_ESP_LDO_CHAN_PSRAM_DOMAIN=2)
#define PSRAM_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static char const TAG[] = "psram";

// What the wrapped IDF functions saw. .bss is zeroed before esp_psram_init, so plain statics work; the wrappers run
// during the tuning itself, so they only store raw numbers (no flash-resident helper is called from them).
static struct {
    bool     phase_seen, dl_seen;
    uint32_t phase_len, phase_end, dl_len, dl_end;
} s_raw;

extern uint32_t __real_mspi_timing_psram_select_best_tuning_phase(const void *configs, uint32_t consecutive_length,
                                                                  uint32_t end, const uint8_t *reference_data, bool is_ddr);
extern uint32_t __real_mspi_timing_psram_select_best_tuning_delayline(const void *configs, uint32_t consecutive_length,
                                                                      uint32_t end, const uint8_t *reference_data, bool is_ddr);

IRAM_ATTR uint32_t __wrap_mspi_timing_psram_select_best_tuning_phase(const void *configs, uint32_t consecutive_length,
                                                                      uint32_t end, const uint8_t *reference_data, bool is_ddr) {
    s_raw.phase_len  = consecutive_length;
    s_raw.phase_end  = end;
    s_raw.phase_seen = true;
    return __real_mspi_timing_psram_select_best_tuning_phase(configs, consecutive_length, end, reference_data, is_ddr);
}

IRAM_ATTR uint32_t __wrap_mspi_timing_psram_select_best_tuning_delayline(const void *configs, uint32_t consecutive_length,
                                                                          uint32_t end, const uint8_t *reference_data,
                                                                          bool is_ddr) {
    s_raw.dl_len  = consecutive_length;
    s_raw.dl_end  = end;
    s_raw.dl_seen = true;
    return __real_mspi_timing_psram_select_best_tuning_delayline(configs, consecutive_length, end, reference_data, is_ddr);
}

typedef struct {
    bool     ran;
    size_t   bytes;
    size_t   bad_bytes;  // both passes
    size_t   bad_plain;  // pattern pass only
    size_t   bad_inv;    // inverted pass only
    size_t   bit_flips;
    size_t   first_bad;
    uint32_t first_xor;
    int64_t  us;
    uintptr_t lo, hi; // extent of the bad bytes (hi == 0: none)
} verify_t;

static psram_tuning_t s_tuning;
static verify_t       s_repeat;
static void          *s_check_addr;
static verify_t       s_verify;
static bool           s_bad;
static char           s_why[64];
static bool           s_policy_done;
static char           s_policy_line[96];

static void tuning_regs(uint8_t *phase_id, uint8_t *dqs, uint8_t *data) {
    *phase_id = (uint8_t)REG_GET_FIELD(IOMUX_MSPI_PIN_PSRAM_DQS_0_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_0_PHASE);
    *dqs      = (uint8_t)MSPI_IOMUX.psram_pin_group.dqs0.reg_psram_dqs_delay_90;
    *data     = (uint8_t)MSPI_IOMUX.psram_pin_group.pin_group0[0].reg_psram_pin_dlc;
}

// Runs from IRAM while the PSRAM timing changes (code and heap live in PSRAM; noinline, or the compiler folds it into
// its XIP caller): the same pin assignment as the IDF's
// mspi_timing_config_psram_set_tuning_delayline
static void NOINLINE_ATTR IRAM_ATTR set_delaylines(uint8_t data, uint8_t dqs) {
    for (int pin = 0; pin < MSPI_LL_PIN_MAX; pin++) {
        mspi_timing_ll_set_delayline((mspi_ll_pin_t)pin, pin == MSPI_LL_PIN_DQS0 || pin == MSPI_LL_PIN_DQS1 ? dqs : data);
    }
}

// Both the console command and the boot one-shot: the other core stalled for the switch. Returns what the registers held
// before and now.
static void apply_delayline(unsigned idx, uint8_t *data, uint8_t *dqs, uint8_t *data_was, uint8_t *dqs_was) {
    uint8_t ph;
    psram_tuning_delays((uint8_t)idx, data, dqs);
    tuning_regs(&ph, dqs_was, data_was);
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    int                 other = xPortGetCoreID() == 0 ? 1 : 0;
    portENTER_CRITICAL(&mux);
    esp_cpu_stall(other);
    set_delaylines(*data, *dqs);
    esp_cpu_unstall(other);
    portEXIT_CRITICAL(&mux);
    tuning_regs(&ph, dqs, data);
}

void psram_boot_set_delayline(char const *idx_text) {
    unsigned idx;
    if (!psram_tuning_dl_args(idx_text, &idx) || idx >= PSRAM_DELAYLINES) {
        console_reply("psram dl <idx 0..%d> yes (15 = no delay; below: DQS delayed, above: data delayed). Bench only: a bad "
                      "idx corrupts code and data in PSRAM",
                      PSRAM_DELAYLINES - 1);
        return;
    }
    // Only where the boot tuning saw the CPU path pass: outside it the code running this (XIP from PSRAM) and every
    // DMA buffer go bad at once
    if (!psram_tuning_dl_in_window(&s_tuning, idx)) {
        console_reply("psram dl: idx %u refused, outside the boot tuning window %u..%u%s", idx, psram_tuning_dl_start(&s_tuning),
                      s_tuning.dl_end, psram_tuning_fell_back(&s_tuning) ? " (tuning fell back)" : "");
        return;
    }
    // A wrong idx must not reach the card: FatFs writes from PSRAM-backed caches and buffers
    if (psram_dma_sd_mounted()) {
        console_reply("psram dl: refused while the SD card is mounted (fw sd unmount first)");
        return;
    }
    uint8_t data, dqs, data_was, dqs_was;
    apply_delayline(idx, &data, &dqs, &data_was, &dqs_was);
    console_reply("psram delayline idx %u set live: data %u, dqs %u (was data %u, dqs %u); not stored, a reboot restores the IDF's choice",
                  idx, data, dqs, data_was, dqs_was);
}

// ---- one-shot override at boot (#121 E2) ----

static char const ONESHOT_KEY[] = "psram_dl1";
static bool       s_os_applied;
static unsigned   s_os_idx;
static verify_t   s_os_verify, s_os_repeat;
static char       s_os_line[112] = "none armed";

// "psram dlboot <idx> yes | clear": arms (or disarms) the override for the NEXT boot only
void psram_boot_oneshot_arm(char const *args) {
    nvs_handle_t nvs;
    unsigned     idx;
    bool         clear = strstr(args, "clear") != NULL;
    if (!clear && !psram_tuning_dl_args(args, &idx)) {
        console_reply("psram dlboot <idx 0..%d> yes | clear: use delayline idx for the NEXT boot only (consumed at that boot, "
                      "never repeated; refused there when outside that boot's tuning window). Last boot: %s",
                      PSRAM_DELAYLINES - 1, s_os_line);
        return;
    }
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        console_reply("psram dlboot: NVS not available");
        return;
    }
    esp_err_t err;
    if (clear) {
        err = nvs_erase_key(nvs, ONESHOT_KEY);
        err = err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    } else if (idx >= PSRAM_DELAYLINES || !psram_tuning_dl_in_window(&s_tuning, idx)) {
        nvs_close(nvs);
        console_reply("psram dlboot: idx %u refused, outside this boot's tuning window %u..%u (the window moves between boots, "
                      "the apply checks again)",
                      idx, psram_tuning_dl_start(&s_tuning), s_tuning.dl_end);
        return;
    } else {
        psram_oneshot_t rec = {PSRAM_ONESHOT_MAGIC, idx};
        err                 = nvs_set_blob(nvs, ONESHOT_KEY, &rec, sizeof(rec));
    }
    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    if (err != ESP_OK) {
        console_reply("psram dlboot: NVS write failed (%s)", esp_err_to_name(err));
    } else if (clear) {
        console_reply("psram dlboot: cleared");
    } else {
        console_reply("psram dlboot: idx %u armed for the next boot only (fw reboot)", idx);
    }
}

// Consume first, then apply: a crash or a wrong idx leaves the next boot on the IDF's choice. Returns the record when one
// was armed.
static bool oneshot_take(psram_oneshot_t *rec) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(*rec);
    bool   have = nvs_get_blob(nvs, ONESHOT_KEY, rec, &size) == ESP_OK && size == sizeof(*rec);
    if (have && (nvs_erase_key(nvs, ONESHOT_KEY) != ESP_OK || nvs_commit(nvs) != ESP_OK)) {
        have = false; // cannot prove it is gone: do not apply it
        snprintf(s_os_line, sizeof(s_os_line), "ignored: could not consume the record");
        ESP_LOGE(TAG, "one-shot delayline: %s", s_os_line);
    }
    nvs_close(nvs);
    return have;
}

static void dense_check_into(verify_t *first, verify_t *again);

void psram_boot_oneshot_apply(void) {
    psram_oneshot_t rec;
    if (!oneshot_take(&rec)) {
        return;
    }
    psram_oneshot_verdict_t v = psram_oneshot_verdict(&rec, &s_tuning);
    if (v != PSRAM_ONESHOT_APPLY) {
        snprintf(s_os_line, sizeof(s_os_line), "idx %u %s (window %u..%u, boot choice idx %u)", (unsigned)rec.idx,
                 psram_oneshot_verdict_text(v), psram_tuning_dl_start(&s_tuning), s_tuning.dl_end, s_tuning.dl_idx);
        ESP_LOGW(TAG, "one-shot delayline %s", s_os_line);
        return;
    }
    uint8_t data, dqs, data_was, dqs_was;
    apply_delayline(rec.idx, &data, &dqs, &data_was, &dqs_was);
    s_os_applied = true;
    s_os_idx     = rec.idx;
    dense_check_into(&s_os_verify, &s_os_repeat);
    snprintf(s_os_line, sizeof(s_os_line), "idx %u APPLIED (data %u, dqs %u; boot choice idx %u): dense check %u bad bytes, again %u",
             (unsigned)rec.idx, data, dqs, s_tuning.dl_idx, (unsigned)s_os_verify.bad_bytes, (unsigned)s_os_repeat.bad_bytes);
    ESP_LOGW(TAG, "one-shot delayline %s", s_os_line);
}

static void line_flush(uint8_t *mem, size_t len) {
    esp_cache_msync(mem, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

// Dense: every byte of the block written and read back as pattern and as its inverse, from the RAM (cache dropped)
static void dense_run(uint8_t *mem, size_t len, verify_t *v) {
    static uint32_t const masks[2] = {0, 0xFFFFFFFFu};
    int64_t               start    = esp_timer_get_time();
    *v                             = (verify_t){0};
    for (int m = 0; m < 2; m++) {
        psram_pattern_fill_x(mem, len, (uintptr_t)mem, masks[m]);
        line_flush(mem, len);
        psram_check_t r = psram_pattern_check_x(mem, len, (uintptr_t)mem, masks[m]);
        if (r.bad_bytes && !v->bad_bytes) {
            v->first_bad = r.first_bad;
            v->first_xor = r.first_xor;
        }
        if (r.bad_bytes) {
            uintptr_t blo = (uintptr_t)mem + r.first_bad, bhi = (uintptr_t)mem + r.last_bad;
            v->lo = !v->hi || blo < v->lo ? blo : v->lo;
            v->hi = bhi > v->hi ? bhi : v->hi;
        }
        v->bad_bytes += r.bad_bytes;
        v->bit_flips += r.bit_flips;
        *(m ? &v->bad_inv : &v->bad_plain) = r.bad_bytes;
    }
    v->us    = esp_timer_get_time() - start;
    v->bytes = len;
    v->ran   = true;
}

static void dense_check_into(verify_t *first, verify_t *again) {
    size_t   len = CHECK_BYTES;
    uint8_t *mem = heap_caps_aligned_alloc(LINE, len, PSRAM_CAPS);
    if (!mem) {
        len = len / 2;
        mem = heap_caps_aligned_alloc(LINE, len, PSRAM_CAPS);
    }
    if (!mem) {
        ESP_LOGW(TAG, "boot check: no PSRAM block for the dense check");
        return;
    }
    dense_run(mem, len, first);
    s_check_addr = mem;
    if (first->bad_bytes) { // the same block again: the same bad bytes point at the place, new ones at the moment
        dense_run(mem, len, again);
    }
    heap_caps_free(mem);
}

static void dense_check(void) {
    dense_check_into(&s_verify, &s_repeat);
}

// "fw psram test <KB> dense": the boot check on demand, with the bad bytes per pass
void psram_boot_dense_test(unsigned kb) {
    uint8_t *mem = heap_caps_aligned_alloc(LINE, (size_t)kb * 1024, PSRAM_CAPS);
    if (!mem) {
        console_reply("psram dense test: no PSRAM for %u KB", kb);
        return;
    }
    verify_t v;
    dense_run(mem, (size_t)kb * 1024, &v);
    console_reply("psram dense test %u KB at %p: pattern %u bad bytes, inverted %u bad bytes, %u single-bit words, first bad +%u xor %08x, %u ms",
                  kb, (void *)mem, (unsigned)v.bad_plain, (unsigned)v.bad_inv, (unsigned)v.bit_flips, (unsigned)v.first_bad,
                  (unsigned)v.first_xor, (unsigned)(v.us / 1000));
    heap_caps_free(mem);
}

// The LP register carrier (#121 spike, now the speed switch's way to reach PSRAM init): PSRAM init reads and clears it, the
// word it found is psram_speed_carrier_raw(). "fw psram carrier" only shows it.
static void carrier_log_boot(void) {
    uint32_t raw = psram_speed_carrier_raw();
    uint16_t v;
    if (psram_carrier_unpack(raw, &v)) {
        ESP_LOGI(TAG, "carrier at boot: magic ok, value %u", (unsigned)v);
    } else {
        ESP_LOGI(TAG, "carrier at boot: empty (raw %08x)", (unsigned)raw);
    }
}

void psram_boot_carrier(char const *args) {
    (void)args;
    uint16_t v;
    uint32_t raw = psram_speed_carrier_raw();
    bool     ok  = psram_carrier_unpack(raw, &v);
    char     a[24];
    snprintf(a, sizeof(a), ok ? "magic ok, value %u" : "empty", (unsigned)v);
    console_reply("psram carrier (LP_STORE15) at PSRAM init: %s (raw %08x); set by fw psram speed, cleared by that init", a, (unsigned)raw);
}

void psram_boot_check(void) {
    carrier_log_boot();
    if (s_raw.phase_seen) {
        psram_tuning_note_phase(&s_tuning, s_raw.phase_len, s_raw.phase_end);
    }
    if (s_raw.dl_seen) {
        psram_tuning_note_delayline(&s_tuning, s_raw.dl_len, s_raw.dl_end);
    }
    char text[128];
    psram_tuning_format(text, sizeof(text), &s_tuning);
    uint8_t ph, dqs, data;
    tuning_regs(&ph, &dqs, &data);
    ESP_LOGI(TAG, "tuning: %s", text);
    ESP_LOGI(TAG, "tuning regs: phase id %u, dqs delay %u, data delay %u", ph, dqs, data);
    dense_check();
    s_bad = psram_boot_verdict(&s_tuning, s_verify.bad_bytes, s_why, sizeof(s_why));
    ESP_LOG_LEVEL(
        s_bad ? ESP_LOG_WARN : ESP_LOG_INFO,
        TAG,
        "boot check: %u KB dense at %p (pattern + inverted), %u bad bytes (pattern %u, inverted %u; %u single-bit "
        "words, first +%u), %u ms; same block again: %u bad: %s%s%s",
        (unsigned)(s_verify.bytes >> 10),
        s_check_addr,
        (unsigned)s_verify.bad_bytes,
        (unsigned)s_verify.bad_plain,
        (unsigned)s_verify.bad_inv,
        (unsigned)s_verify.bit_flips,
        (unsigned)s_verify.first_bad,
        (unsigned)(s_verify.us / 1000),
        (unsigned)s_repeat.bad_bytes,
        s_bad ? "BAD (" : "ok",
        s_bad ? s_why : "",
        s_bad ? ")" : ""
    );
}

static uint32_t wall_s(void) {
    time_t now = time(NULL);
    return now > 1735689600 ? (uint32_t)now : 0; // before this the clock is not set (#63)
}

static void policy_line(psram_retry_t const *st, bool reboot) {
    snprintf(s_policy_line, sizeof(s_policy_line), "%s, retries %u since the last good boot, %s", s_bad ? "bad boot" : "good boot",
             (unsigned)st->retries,
             !s_bad ? "nothing to do" : reboot ? (PSRAM_GUARD_REBOOT_ON_BAD ? "rebooting once" : "would reboot once (log only)")
                                               : "no reboot (retry used, keep booting)");
}

void psram_boot_policy(void) {
    psram_retry_t st = {0};
    nvs_handle_t  nvs;
    bool          have_nvs = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK;
    if (have_nvs) {
        size_t size = sizeof(st);
        if (nvs_get_blob(nvs, RETRY_KEY, &st, &size) != ESP_OK || size != sizeof(st)) {
            st = (psram_retry_t){0};
        }
    }
    bool reboot = psram_retry_decide(&st, s_bad, wall_s());
    if (have_nvs) {
        nvs_set_blob(nvs, RETRY_KEY, &st, sizeof(st));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    policy_line(&st, reboot);
    s_policy_done = true;
    if (!s_bad) {
        return;
    }
    ESP_LOGW(TAG, "bad boot: %s; %s", s_why, s_policy_line);
    system_restart_note(s_why);
    if (reboot && PSRAM_GUARD_REBOOT_ON_BAD) {
        system_restart("psram retune");
    }
}

static void print_ldo(void) {
    uint8_t dref_e = (uint8_t)EFUSE.rd_mac_sys_2.ldo_vo2_dref;
    uint8_t mul_e  = (uint8_t)EFUSE.rd_mac_sys_3.ldo_vo2_mul;
    uint8_t dref   = (uint8_t)PMU.ext_ldo[LDO_PSRAM_IDX].pmu_ext_ldo_ana.dref;
    uint8_t mul    = (uint8_t)PMU.ext_ldo[LDO_PSRAM_IDX].pmu_ext_ldo_ana.mul;
    unsigned vref  = dref < 9 ? 500u + 50u * dref : 1000u + 100u * (dref - 9u);
    console_reply("psram ldo: eFuse block v%u, vo2 dref %u mul %u (calibrated pair %s); live unit 1 (VO2) dref %u mul %u, tieh %u, "
                  "uncalibrated nominal ~%u mV",
                  (unsigned)efuse_hal_blk_version(), dref_e, mul_e, dref_e && mul_e ? "set" : "NOT set", dref, mul,
                  (unsigned)PMU.ext_ldo[LDO_PSRAM_IDX].pmu_ext_ldo.tieh, vref * (4u + mul) / 4u);
}

size_t psram_boot_tuning_text(char *out, size_t cap) {
    return psram_tuning_format(out, cap, &s_tuning);
}

void psram_boot_print(void) {
    char text[128];
    psram_tuning_format(text, sizeof(text), &s_tuning);
    console_reply("psram tuning: %s", text);
    uint8_t ph, dqs, data, edata, edqs;
    tuning_regs(&ph, &dqs, &data);
    psram_tuning_delays(s_os_applied ? (uint8_t)s_os_idx : s_tuning.dl_idx, &edata, &edqs);
    bool match = ph == s_tuning.phase_id && dqs == edqs && data == edata;
    console_reply("psram tuning regs: phase id %u, dqs delay %u, data delay %u (%s)", ph, dqs, data,
                  match ? "match the captured choice" : "DIFFER from the captured choice");
    console_reply("psram boot check: %u KB dense at %p (pattern + inverted), %u bad bytes (pattern %u, inverted %u), %u single-bit words, first bad +%u xor %08x, %u ms, same block again: %u bad: %s%s",
                  (unsigned)(s_verify.bytes >> 10), s_check_addr, (unsigned)s_verify.bad_bytes, (unsigned)s_verify.bad_plain,
                  (unsigned)s_verify.bad_inv, (unsigned)s_verify.bit_flips, (unsigned)s_verify.first_bad, (unsigned)s_verify.first_xor, (unsigned)(s_verify.us / 1000),
                  (unsigned)s_repeat.bad_bytes, s_bad ? "BAD " : "ok", s_bad ? s_why : "");
    console_reply("psram one-shot delayline (this boot): %s", s_os_line);
    console_reply("psram boot policy: %s", s_policy_done ? s_policy_line : "not evaluated yet");
    print_ldo();
}

bool psram_boot_bad(char *why, size_t cap) {
    if (cap) {
        snprintf(why, cap, "%s", s_bad ? s_why : "");
    }
    return s_bad;
}
