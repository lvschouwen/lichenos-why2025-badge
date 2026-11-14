#include "c6link.h"
#include "board_sdmmc.h"

#include "c6_boot_policy.h"
#include "c6_hooks_priv.h"
#include "c6link_priv.h"
#include "c6_flasher.h"
#include "c6_image.h"
#include "c6_paths.h"
#include "fw_console.h"
#include "nvs_fail.h"
#include "esp_hosted.h"
#include "esp_hosted_frame_pool.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "hal/rtc_io_ll.h"
#include "c6_reg.h"
#include "reg.h"
#include <sys/stat.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

static char const TAG[] = "c6link";

// Written by the boot attach and the supervisor (c6_sup.c, #14); read from any task
static atomic_bool  link_up;

bool c6link_up(void) {
    return atomic_load(&link_up);
}

void c6link_set_link(bool up) {
    atomic_store(&link_up, up);
}

// The NVS flag "fw c6 reflash" sets; read and cleared here so a forced reflash happens once
static bool take_force_flag(void) {
    nvs_handle_t nvs;
    uint8_t      force = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    if (nvs_get_u8(nvs, "c6_force", &force) == ESP_OK) {
        if (fw_nvs_erased(TAG, "c6_force", nvs_erase_key(nvs, "c6_force"))) {
            fw_nvs_ok(TAG, "c6_force", nvs_commit(nvs));
        }
    }
    nvs_close(nvs);
    return force != 0;
}

static atomic_bool updating; // a firmware update is being received: no scans (#76); read by c6sup and Wi-Fi (#14)

void c6link_set_updating(void) {
    atomic_store(&updating, true);
}

// Power off (#65): C6 EN low and held there through the P4's deep sleep (GPIO12 is an LP IO), so the C6, its radios
// and the TCA8418 on the same reset draw nothing. Last thing before esp_deep_sleep_start: the link is gone after it.
void c6link_hold_in_reset(void) {
    atomic_store(&updating, true); // the supervisor must not take this for an outage
    gpio_set_level(GPIO_NUM_12, 0);
    gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT); // it is already (the reset line); low before it drives
    if (gpio_hold_en(GPIO_NUM_12) != ESP_OK) {
        ESP_LOGW(TAG, "C6 EN: no pad hold, the C6 may start again in deep sleep");
    }
}

// registry hold.gpio12 / hold.gpio13 (#319): the pad-hold latch of the two LP pads that steer the C6 (EN, boot strap),
// read from the same register gpio_hold_en sets (IDF has no getter). Which register depends on the chip revision the
// image is built for, exactly as in gpio_ll_hold_en: rev >= 3 LP_SYS.pad_rtc_hold_ctrl0, below that LP_IOMUX.lp_pad_hold.
// Live register read, no bus traffic. On this build (rev 1+) a hold is lost across deep sleep, so after a wake it reads off.
reg_status_t c6link_reg_hold(reg_entry_t const *e, reg_value_t *out) {
    int      pad  = e->id == REG_ID_HOLD_GPIO12 ? 12 : 13;
#if CONFIG_ESP_REV_MIN_FULL >= 300 // what HAL_CONFIG(CHIP_SUPPORT_MIN_REV) is
    uint32_t mask = LP_SYS.pad_rtc_hold_ctrl0.pad_rtc_hold_ctrl0;
#else
    uint32_t mask = HAL_FORCE_READ_U32_REG_FIELD(LP_IOMUX.lp_pad_hold, reg_lp_gpio_hold);
#endif
    out->u = (mask >> pad) & 1u;
    return REG_OK;
}

bool c6link_updating(void) {
    return atomic_load(&updating);
}

static void cmd_c6log(char const *args) {
    uint32_t ms = *args ? (uint32_t)strtoul(args, NULL, 10) : 10000;
    if (ms == 0 || ms > 60000) {
        console_reply("c6log [1-60000 ms]");
        return;
    }
    c6_log_console(ms);
}

static bool nvs_write_boot_state(char const *key, bool verify);
static void cmd_c6_status(void);

static void cmd_c6(char const *args) {
    if (c6_sup_command(args)) { // reset, reset rom, reattach (#14)
        return;
    }
    if (strcmp(args, "verify") == 0) {
        if (!nvs_write_boot_state(NULL, true)) {
            console_reply("c6: can't save the verify flag");
            return;
        }
        console_reply("C6 is checked fully during the next boot, rebooting");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        system_restart_asked("C6 verify requested");
    }
    if (strcmp(args, "reflash") != 0) {
        cmd_c6_status();
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        console_reply("c6: can't save the reflash flag");
        return;
    }
    bool saved = fw_nvs_ok(TAG, "c6_force", nvs_set_u8(nvs, "c6_force", 1)) &&
                 fw_nvs_ok(TAG, "c6_force", nvs_commit(nvs));
    nvs_close(nvs); // on the error path too (#125)
    if (!saved) {
        console_reply("c6: can't save the reflash flag");
        return;
    }
    console_reply("C6 is flashed again during the next boot, rebooting");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    system_restart_asked("C6 reflash requested");
}


// ---- Skipping the flasher check on warm restarts (#36) ----
// NVS (FW_NVS_NAMESPACE): c6_ok_key = bundle key stored after the last successful verify/flash, c6_verify = the last
// attach after a skip failed, so the next boot checks fully. Only a software reset can skip; the full path never restarts.
static char const KEY_OK[]     = "c6_ok_key";
static char const KEY_VERIFY[] = "c6_verify";

// How long IDENT may take after the attach (#36): the C6 app has only just booted (the hosted EN reset), and its LoRa
// service registers for peer data a moment after ESP-Hosted is up. Only a C6 that does not answer waits all of it.
#define C6_IDENT_WAIT_MS 2000

static struct {
    char const *plan;     // "skip" / "full"
    char const *why;
    uint32_t    c6_ms;    // time spent in the C6 step this boot
    uint32_t    ident_ms; // of which IDENT
    c6_ident_t  ident;
    char        key[C6_BUNDLE_KEY_MAX];
} boot_info = {.plan = "?", .why = "?"};

static bool (*ident_fn)(uint8_t out[C6LINK_IDENT_LEN], uint32_t timeout_ms);

_Static_assert(C6LINK_IDENT_LEN == C6_IDENT_SHA_LEN && C6_IDENT_SHA_LEN == C6_APP_SHA_LEN, "IDENT sha size");

static bool (*ident_ready_fn)(void);

void c6link_set_ident_ready(bool (*ready)(void)) {
    ident_ready_fn = ready;
}

void c6link_set_ident(bool (*ident)(uint8_t out[C6LINK_IDENT_LEN], uint32_t timeout_ms)) {
    ident_fn = ident;
}

// md5 text and size of one bundle file, the same files the flasher compares (no hashing of the 1 MB app)
static bool read_bundle_file(char const *name, char md5[40], unsigned long *size) {
    char path[64], md5_name[40];
    snprintf(md5_name, sizeof(md5_name), "%s.md5", name);
    if (!c6_image_path(name, path, sizeof(path))) {
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return false;
    }
    *size = (unsigned long)st.st_size;
    if (!c6_image_path(md5_name, path, sizeof(path))) {
        return false;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    size_t n = fread(md5, 1, 39, f);
    fclose(f);
    md5[n] = '\0';
    return n >= 32;
}

static bool current_bundle_key(char *out, size_t cap) {
    char          m[3][40];
    unsigned long sz[3];
    static char const *const names[3] = {"bootloader.bin", "partitions.bin", "firmware.bin"};
    for (int i = 0; i < 3; i++) {
        if (!read_bundle_file(names[i], m[i], &sz[i])) {
            return false;
        }
    }
    return c6_bundle_key(m[0], m[1], m[2], sz[0], sz[1], sz[2], out, cap);
}

// The bundled app's ELF sha256, from the first bytes of its .bin (no hashing of the 1 MB app)
static bool bundle_app_sha(uint8_t out[C6_APP_SHA_LEN]) {
    char path[64];
    if (!c6_image_path("firmware.bin", path, sizeof(path))) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t head[C6_APP_SHA_NEEDED];
    size_t  n = fread(head, 1, sizeof(head), f);
    fclose(f);
    return c6_app_elf_sha256(head, n, out);
}

// After the attach: does the running C6 app match the bundle? Any doubt is "not proven" (#36).
static c6_ident_t check_ident(uint32_t *ms_out) {
    uint8_t expected[C6_APP_SHA_LEN], running[C6_IDENT_SHA_LEN];
    bool    have_expected = bundle_app_sha(expected);
    int64_t t0            = esp_timer_get_time();
    bool    answered      = ident_fn && ident_fn(running, C6_IDENT_WAIT_MS);
    *ms_out               = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    c6_ident_t r          = c6_ident_check(answered ? running : NULL, have_expected ? expected : NULL);
    ESP_LOG_LEVEL(
        r == C6_IDENT_MATCH ? ESP_LOG_INFO : ESP_LOG_WARN,
        TAG,
        "C6 IDENT %s after %u ms",
        c6_ident_name(r),
        (unsigned)*ms_out
    );
    return r;
}

static c6_ident_t rt_ident = C6_IDENT_NOT_ASKED; // the last runtime re-attach's IDENT (#36), for `fw c6`
static uint32_t   rt_ident_ms;

static void nvs_read_boot_state(char *stored, size_t cap, bool *verify) {
    nvs_handle_t nvs;
    uint8_t      v = 0;
    stored[0]      = '\0';
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = cap;
    if (nvs_get_str(nvs, KEY_OK, stored, &len) != ESP_OK) {
        stored[0] = '\0';
    }
    nvs_get_u8(nvs, KEY_VERIFY, &v);
    nvs_close(nvs);
    *verify = v != 0;
}

// Writes only what changes (key == NULL erases it): the NVS flash wears per write. True only when everything asked for
// is committed.
static bool nvs_write_boot_state(char const *key, bool verify) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    bool   ok                     = true;
    char   cur[C6_BUNDLE_KEY_MAX] = "";
    size_t len                    = sizeof(cur);
    bool   have                   = nvs_get_str(nvs, KEY_OK, cur, &len) == ESP_OK;
    if (key && (!have || strcmp(cur, key) != 0)) {
        ok = fw_nvs_ok(TAG, KEY_OK, nvs_set_str(nvs, KEY_OK, key)) && ok;
    } else if (!key && have) {
        ok = fw_nvs_erased(TAG, KEY_OK, nvs_erase_key(nvs, KEY_OK)) && ok;
    }
    uint8_t v     = 0;
    bool    vhave = nvs_get_u8(nvs, KEY_VERIFY, &v) == ESP_OK;
    if (verify && (!vhave || !v)) {
        ok = fw_nvs_ok(TAG, KEY_VERIFY, nvs_set_u8(nvs, KEY_VERIFY, 1)) && ok;
    } else if (!verify && vhave) {
        ok = fw_nvs_erased(TAG, KEY_VERIFY, nvs_erase_key(nvs, KEY_VERIFY)) && ok;
    }
    ok = fw_nvs_ok(TAG, KEY_OK, nvs_commit(nvs)) && ok;
    nvs_close(nvs);
    return ok;
}

static atomic_bool s_bundle_changed;

void c6link_bundle_changed(void) {
    atomic_store(&s_bundle_changed, true);
    nvs_write_boot_state(NULL, false);
}

bool c6link_bundle_pending(void) {
    return atomic_load(&s_bundle_changed);
}

// The C6 was lost at runtime and the badge restarts for it (#14): check it fully during the next boot, keeping the key
bool c6link_set_verify_flag(void) {
    char stored[C6_BUNDLE_KEY_MAX];
    bool verify = false;
    nvs_read_boot_state(stored, sizeof(stored), &verify);
    return nvs_write_boot_state(stored[0] ? stored : NULL, true);
}

// After a runtime re-attach (#14), once LoRa is back: the same proof as at boot. Anything but a match is not proven, so
// the next boot checks the C6 fully (the stored key stays); no restart here, the C6 works and the badge is in use.
void c6link_check_ident_runtime(void) {
    if (c6link_updating()) {
        return; // LoRa is stopped for the update: lora_ident would borrow a callback nobody gives back
    }
    if (ident_ready_fn && !ident_ready_fn()) {
        rt_ident = C6_IDENT_NOT_ASKED; // no LoRa task (radio off): nobody can ask, which proves nothing either way
        return;
    }
    rt_ident = check_ident(&rt_ident_ms);
    if (rt_ident != C6_IDENT_MATCH) {
        ESP_LOGW(TAG, "C6 app not proven after the re-attach: full check on the next boot");
        c6link_set_verify_flag();
    }
}

// The Wi-Fi frame pools of the link (#260): how deep they got, and how often a frame waited or was dropped
__attribute__((noinline)) static void report_frame_pools(void) { // own frame: cmd_c6_status is near the stack warn line
    esp_hosted_frame_pool_stats_t st;
    char                          line[160];
    for (int tx = 1; tx >= 0; tx--) {
        if (!esp_hosted_frame_pool_stats(tx, &st)) {
            console_reply("c6: frame pools off (per-frame malloc)");
            return;
        }
        esp_hosted_frame_pool_fmt(line, sizeof(line), tx ? "c6: pool tx" : "c6: pool rx", &st);
        console_reply("%s", line);
    }
}

bool c6link_frame_pool(bool tx, uint16_t *in_use, uint16_t *peak, uint32_t *waits, uint32_t *drops) {
    esp_hosted_frame_pool_stats_t st;
    bool                          on = esp_hosted_frame_pool_stats(tx, &st); // zeros when off
    *in_use = st.in_use;
    *peak   = st.peak;
    *waits  = st.waits;
    *drops  = st.drops;
    return on;
}

static void cmd_c6_status(void) {
    char stored[C6_BUNDLE_KEY_MAX];
    bool verify = false;
    nvs_read_boot_state(stored, sizeof(stored), &verify);
    console_reply("c6: this boot %s (%s), C6 step %u ms, IDENT %s (%u ms)", boot_info.plan, boot_info.why,
                  (unsigned)boot_info.c6_ms, c6_ident_name(boot_info.ident), (unsigned)boot_info.ident_ms);
    if (rt_ident != C6_IDENT_NOT_ASKED) {
        console_reply("c6: last re-attach IDENT %s (%u ms)", c6_ident_name(rt_ident), (unsigned)rt_ident_ms);
    }
    console_reply("c6: stored key %s, verify flag %d", stored[0] ? stored : "(none)", verify);
    char line[C6W_REASON_MAX + 64];
    c6_sup_watch_line(line, sizeof(line));
    console_reply("%s", line);
    c6_sup_hb_line(line, sizeof(line));
    console_reply("%s", line);
    report_frame_pools();
    console_reply("c6 reflash | verify (full check on the next boot) | reset [rom] (pulse C6 EN now) | reattach (#14)");
}

// What to do about the flasher this boot. Returns true to run it.
static bool plan_full_check(bool force, char *key, size_t key_cap) {
    char stored[C6_BUNDLE_KEY_MAX];
    bool verify = false;
    nvs_read_boot_state(stored, sizeof(stored), &verify);
    if (!current_bundle_key(key, key_cap)) {
        key[0] = '\0';
    }
    c6_reset_kind_t kind = esp_reset_reason() == ESP_RST_SW ? C6_RST_SOFTWARE : C6_RST_OTHER;
    c6_plan_t       plan = c6_boot_plan(kind, force, verify, c6_hooks_image_on_trial(), stored, key, &boot_info.why);
    boot_info.plan       = plan == C6_PLAN_SKIP ? "skip" : "full";
    return plan == C6_PLAN_FULL_CHECK;
}

// P4 GPIO13 is the C6 GPIO9 strap: low at a reset means download mode. esp_hosted now resets the C6 through EN (GPIO12), so
// the strap has to be high first, on every path (the flasher may have bailed out early, #2). The level is set before the
// pin becomes an output so it never glitches. Low only for "fw c6 reset rom" (#14).
void hold_c6_boot(int level) {
    gpio_set_level(GPIO_NUM_13, level);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_13,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    gpio_set_level(GPIO_NUM_13, level);
}

static atomic_bool s_flashing;

bool c6link_flashing(void) {
    return atomic_load(&s_flashing);
}

bool c6link_start(void) {
    console_register("c6", "c6 [reflash | verify | reset [rom] | reattach]", cmd_c6);
    c6_sup_start();

    c6_hooks_step_begin();
    int64_t   t0    = esp_timer_get_time();
    bool      force = take_force_flag();
    char      key[C6_BUNDLE_KEY_MAX];
    bool      full = plan_full_check(force, key, sizeof(key));
    esp_err_t err  = ESP_OK;
    if (full) {
        ESP_LOGI(TAG, "C6 flasher check runs (%s)", boot_info.why);
        atomic_store(&s_flashing, true);
        err = flash_c6_if_needed(force);
        atomic_store(&s_flashing, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "C6 image check: %s (going on with what the C6 has)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "C6 bundle unchanged, ROM check skipped (SW reset)");
    }
    // Only after the flash check (#250): c6log borrows UART1 shared, and must never overlap the flasher's exclusive lend
    console_register("c6log", "c6log [ms] (copy the C6's console into the log)", cmd_c6log);
    // The C6 was reset into its application by the flasher; only now may ESP-Hosted talk to it
    hold_c6_boot(1);
    // esp_hosted_init adds SDMMC slot 1 to the controller the SD card shares (#60): under its lock, not the connect
    board_sdmmc_lock(UINT32_MAX);
    bool inited = esp_hosted_init() == 0;
    board_sdmmc_unlock();
    bool attached = inited && esp_hosted_connect_to_slave() == 0;
    // IDENT on both paths: a full boot stores the key only when the running C6 proved the bundled app, so a later skip
    // rests on a C6 that answered IDENT, never on an older image that cannot
    boot_info.ident = attached ? check_ident(&boot_info.ident_ms) : C6_IDENT_NOT_ASKED;
    bool ident_ok   = boot_info.ident == C6_IDENT_MATCH;
    // The key is stored only after a successful attach and IDENT; a failed skip sets the verify flag (full check next
    // boot)
    c6_state_write_t w         = c6_state_after_attach(full, attached, ident_ok, err == ESP_OK, key[0] != '\0');
    bool             persisted = w.write && nvs_write_boot_state(w.store_key ? key : NULL, w.verify);
    // A skip is a bet that the C6 is fine; when it loses, check fully next boot. Never restart when that could not be
    // saved (next boot would skip again: a loop without Wi-Fi OTA); the full path never restarts. Never on a trial
    // image either: those always take the full path.
    if (c6_restart_after_attach(full, attached, ident_ok, persisted)) {
        ESP_LOGW(TAG, "C6 %s after a skipped check: full check on the next boot",
                 attached ? "app not proven by IDENT" : "did not attach");
        // Gated (#304), but a trial image never gets here (full path), so it does not wait in practice; if it did, the
        // boot step would wait up to OTA_RESTART_GATE_MAX_S of uptime
        system_restart(attached ? "C6 IDENT failed after skip" : "C6 attach failed after skip");
    }
    if (!attached) {
        ESP_LOGE(TAG, "no connection to the C6 over ESP-Hosted");
        c6_hooks_step_end(false, "no ESP-Hosted link");
        c6_hooks_no_link(); // the Wi-Fi and clock steps: skipped, "no C6"
        return false;
    }
    atomic_store(&link_up, true);
    c6_sup_attached(); // from now on c6sup notices a C6 reset and recovers from it (#14)
    boot_info.c6_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "C6 step %s: %u ms", boot_info.plan, (unsigned)boot_info.c6_ms);
    char note[C6LINK_STEP_NOTE_LEN];
    char const *how = !full ? "check skipped, " : err != ESP_OK ? "image check failed, " : ""; // a skip has no err
    snprintf(note, sizeof(note), "%s%s%u ms", how, ident_ok ? "" : "not proven, ", (unsigned)boot_info.c6_ms);
    c6_hooks_step_end(true, note);
    if (!c6_hooks_attached()) { // the Wi-Fi service (lichen_net) brings the driver up
        ESP_LOGW(TAG, "no Wi-Fi service registered: Wi-Fi not started");
    }
    return true;
}
