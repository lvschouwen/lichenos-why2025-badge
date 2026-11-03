#include "psram_speed.h"

#include "psram_boot.h"
#include "psram_speed_logic.h"
#include "psram_tuning.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "nvs.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

#include <stdio.h>
#include <string.h>

#if CONFIG_SPIRAM_SPEED_RUNTIME_SELECT
#include "esp_private/mspi_timing_tuning.h"
#endif

#define REC_KEY  "psram_spd"
#define CARRIER  LP_SYSTEM_REG_LP_STORE15_REG // spike #121: survives esp_restart, ota-reboot and an int-WDT reset

static char const TAG[] = "psram";

static psram_speed_rec_t s_rec = {.want = 80};
static bool              s_trial_live;
static bool              s_keep120;    // a clean restart re-arms 120 MHz (RAM only: a crash must come back at 80)
static bool              s_gate_failed;
static bool              s_done;       // psram_speed_boot ran
static void            (*s_pass_cb)(void);

bool psram_speed_supported(void) {
#if CONFIG_SPIRAM_SPEED_RUNTIME_SELECT
    return true;
#else
    return false;
#endif
}

unsigned psram_speed_running(void) {
#if CONFIG_SPIRAM_SPEED_RUNTIME_SELECT
    return g_mspi_psram_speed_mhz;
#else
    return CONFIG_SPIRAM_SPEED;
#endif
}

uint32_t psram_speed_carrier_raw(void) {
#if CONFIG_SPIRAM_SPEED_RUNTIME_SELECT
    return g_mspi_psram_carrier_found; // PSRAM init cleared the register after reading it
#else
    return REG_READ(CARRIER);
#endif
}

static void rec_load(void) {
    nvs_handle_t nvs;
    s_rec = (psram_speed_rec_t){.want = 80};
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t size = sizeof(s_rec);
    if (nvs_get_blob(nvs, REC_KEY, &s_rec, &size) != ESP_OK || size != sizeof(s_rec)) {
        s_rec = (psram_speed_rec_t){.want = 80};
    }
    nvs_close(nvs);
    psram_speed_rec_sane(&s_rec);
}

static bool rec_save(void) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(nvs, REC_KEY, &s_rec, sizeof(s_rec)) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    if (!ok) {
        ESP_LOGE(TAG, "speed record not saved: the next boot may not follow the choice");
    }
    return ok;
}

static void arm_register(bool on) {
    REG_WRITE(CARRIER, on ? ((0xC4A1u << 16) | 120u) : 0u);
}

// esp_restart() and everything built on it (system_restart, update, power menu) run this; a panic, WDT or brownout does
// not, which is what makes those come back at 80.
static void shutdown_hook(void) {
    if (s_keep120) {
        arm_register(true);
    }
}

static void log_rec(char const *what) {
    char text[40];
    psram_speed_value_text(&s_rec, psram_speed_running(), text, sizeof(text));
    ESP_LOGI(TAG, "speed %s: running %u MHz, want %u, trial %u, failed %u (%s)", what, psram_speed_running(), s_rec.want,
             s_rec.trial, s_rec.failed, text);
}

void psram_speed_boot(void) {
    s_done = true;
    rec_load();
    uint32_t raw = psram_speed_carrier_raw();
    ESP_LOGI(TAG, "speed: carrier found at PSRAM init %08x, running %u MHz%s", (unsigned)raw, psram_speed_running(),
             psram_speed_supported() ? "" : " (IDF without the runtime speed patch)");
    psram_speed_rec_t before = s_rec;
    psram_speed_act_t act    = psram_speed_boot_decide(&s_rec, psram_speed_running(), psram_speed_supported(), &s_trial_live);
    if (memcmp(&before, &s_rec, sizeof(s_rec)) != 0) {
        rec_save();
    }
    if (before.trial && !s_rec.trial && s_rec.failed) {
        ESP_LOGE(TAG, "a 120 MHz boot did not get through (crash, watchdog or restart before it passed): back at 80 MHz, "
                      "not tried again until chosen in Settings or with fw psram speed 120");
    }
    s_keep120 = s_trial_live;
    esp_register_shutdown_handler(shutdown_hook);
    log_rec("record");
    if (act == PSRAM_SPEED_ACT_ARM_RESTART) {
        ESP_LOGW(TAG, "speed: 120 MHz wanted, restarting once to boot at 120 MHz");
        arm_register(true);
        esp_restart(); // not system_restart: no app has started yet, nothing to save
    } else if (act == PSRAM_SPEED_ACT_RESTART) {
        ESP_LOGW(TAG, "speed: running 120 MHz against the record, restarting at 80 MHz");
        s_keep120 = false;
        arm_register(false);
        esp_restart();
    }
}

bool psram_speed_trial_pending(void) {
    return s_trial_live;
}

void psram_speed_on_pass(void (*cb)(void)) {
    s_pass_cb = cb;
}

// A failed 120 MHz: the record says 80 and failed, nothing re-arms, the restart lands at 80
static void fail_and_restart(char const *why) {
    s_keep120 = false;
    psram_speed_fail(&s_rec);
    rec_save();
    arm_register(false);
    ESP_LOGE(TAG, "120 MHz failed (%s): restarting at 80 MHz, not tried again until chosen", why);
    system_restart_note("psram 120 failed");
    system_restart("psram 120 failed");
}

void psram_speed_tick(void) {
    if (!s_trial_live) {
        return;
    }
    char why[64];
    bool bad = psram_boot_bad(why, sizeof(why));
    switch (psram_speed_trial_step((unsigned)(esp_timer_get_time() / 1000000), bad, s_gate_failed)) {
        case PSRAM_SPEED_TRIAL_RUNNING: return;
        case PSRAM_SPEED_TRIAL_FAIL: fail_and_restart(bad ? why : "the DMA gate failed"); return;
        case PSRAM_SPEED_TRIAL_PASS:
            s_trial_live = false;
            s_keep120    = true;
            psram_speed_pass(&s_rec);
            rec_save();
            ESP_LOGI(TAG, "120 MHz trial passed (%d s, clean boot check): 120 MHz stays", PSRAM_SPEED_TRIAL_SECONDS);
            if (s_pass_cb) {
                s_pass_cb();
            }
            return;
    }
}

void psram_speed_gate_result(bool failed) {
    if (!failed) {
        return;
    }
    s_gate_failed = true;
    if (psram_speed_running() == 120 && s_done) {
        console_reply("psram: the DMA gate failed at 120 MHz: back at 80 MHz, restarting");
        fail_and_restart("the DMA gate failed");
    }
}

bool psram_speed_choice(unsigned target) {
    if (!psram_speed_supported() || (target != 80 && target != 120)) {
        return false;
    }
    psram_speed_choose(&s_rec, target);
    s_trial_live = false;
    s_keep120    = false; // the restart that follows is not a 120 MHz restart unless the register is armed below
    if (!rec_save()) {
        return false;
    }
    arm_register(target == 120);
    ESP_LOGW(TAG, "speed: %u MHz chosen, the badge restarts", target);
    return true;
}

size_t psram_speed_value(char *out, size_t cap) {
    return psram_speed_value_text(&s_rec, psram_speed_running(), out, cap);
}

unsigned psram_speed_other(void) {
    return s_rec.want == 120 && !s_rec.failed ? 80 : 120;
}

void psram_speed_print(void) {
    char text[40];
    psram_speed_value_text(&s_rec, psram_speed_running(), text, sizeof(text));
    console_reply("psram speed: running %u MHz; %s; want %u, trial %s, failed %s; IDF %s; carrier found at PSRAM init %08x%s",
                  psram_speed_running(), text, s_rec.want, s_trial_live ? "running" : s_rec.trial ? "armed" : "none",
                  s_rec.failed ? "yes" : "no", psram_speed_supported() ? "runtime-selectable" : "NOT patched (0003)",
                  (unsigned)psram_speed_carrier_raw(), s_gate_failed ? "; DMA gate failed this boot" : "");
}

// "fw psram speed [80|120] [yes]": no argument = status; a speed without yes = what would happen
void psram_speed_cmd(char const *args) {
    unsigned target = 0;
    if (sscanf(args, " %u", &target) != 1) {
        psram_speed_print();
        return;
    }
    if (!psram_speed_supported()) {
        console_reply("psram speed: this firmware was built with an IDF without patch 0003, 80 MHz only");
        return;
    }
    if (target != 80 && target != 120) {
        console_reply("psram speed [80|120] [yes]: 80 or 120 MHz, applied by a restart (yes to go on). If 120 MHz fails the "
                      "badge comes back at 80");
        return;
    }
    if (!strstr(args, "yes")) {
        console_reply("psram speed %u: this restarts the badge%s. Add yes to do it (fw psram speed %u yes)", target,
                      target == 120 ? "; if 120 MHz fails (boot check, crash, watchdog within 60 s, or a failed fw psram dma) "
                                       "it comes back at 80 and stays there"
                                    : "",
                      target);
        return;
    }
    if (!psram_speed_choice(target)) {
        console_reply("psram speed: not saved, nothing changed");
        return;
    }
    console_reply("psram speed %u: saved, restarting now", target);
    system_restart("psram speed");
}
