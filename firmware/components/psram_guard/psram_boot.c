#include "psram_boot.h"

#include "psram_pattern.h"
#include "psram_tuning.h"

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "hal/efuse_hal.h"
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

static void dense_check(void) {
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
    dense_run(mem, len, &s_verify);
    s_check_addr = mem;
    if (s_verify.bad_bytes) { // the same block again: the same bad bytes point at the place, new ones at the moment
        dense_run(mem, len, &s_repeat);
    }
    heap_caps_free(mem);
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

void psram_boot_check(void) {
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
    ESP_LOGW(TAG, "tuning: %s", text);
    ESP_LOGW(TAG, "tuning regs: phase id %u, dqs delay %u, data delay %u", ph, dqs, data);
    dense_check();
    s_bad = psram_boot_verdict(&s_tuning, s_verify.bad_bytes, s_why, sizeof(s_why));
    ESP_LOGW(TAG, "boot check: %u KB dense at %p (pattern + inverted), %u bad bytes (pattern %u, inverted %u; %u single-bit words, first +%u), %u ms; same block again: %u bad: %s%s%s",
             (unsigned)(s_verify.bytes >> 10), s_check_addr, (unsigned)s_verify.bad_bytes, (unsigned)s_verify.bad_plain, (unsigned)s_verify.bad_inv, (unsigned)s_verify.bit_flips,
             (unsigned)s_verify.first_bad, (unsigned)(s_verify.us / 1000), (unsigned)s_repeat.bad_bytes, s_bad ? "BAD (" : "ok", s_bad ? s_why : "", s_bad ? ")" : "");
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

void psram_boot_print(void) {
    char text[128];
    psram_tuning_format(text, sizeof(text), &s_tuning);
    console_reply("psram tuning: %s", text);
    uint8_t ph, dqs, data, edata, edqs;
    tuning_regs(&ph, &dqs, &data);
    psram_tuning_delays(s_tuning.dl_idx, &edata, &edqs);
    bool match = ph == s_tuning.phase_id && dqs == edqs && data == edata;
    console_reply("psram tuning regs: phase id %u, dqs delay %u, data delay %u (%s)", ph, dqs, data,
                  match ? "match the captured choice" : "DIFFER from the captured choice");
    console_reply("psram boot check: %u KB dense at %p (pattern + inverted), %u bad bytes (pattern %u, inverted %u), %u single-bit words, first bad +%u xor %08x, %u ms, same block again: %u bad: %s%s",
                  (unsigned)(s_verify.bytes >> 10), s_check_addr, (unsigned)s_verify.bad_bytes, (unsigned)s_verify.bad_plain,
                  (unsigned)s_verify.bad_inv, (unsigned)s_verify.bit_flips, (unsigned)s_verify.first_bad, (unsigned)s_verify.first_xor, (unsigned)(s_verify.us / 1000),
                  (unsigned)s_repeat.bad_bytes, s_bad ? "BAD " : "ok", s_bad ? s_why : "");
    console_reply("psram boot policy: %s", s_policy_done ? s_policy_line : "not evaluated yet");
    print_ldo();
}
