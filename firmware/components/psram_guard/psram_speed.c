#include "psram_speed.h"

#include "psram_boot.h"
#include "psram_dma_cmd.h"
#include "psram_speed_logic.h"
#include "psram_tuning.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
static bool              s_keep120;    // a clean restart re-arms 120 MHz: only after a passed trial (RAM only: a crash comes back at 80)
static bool              s_gate_failed;
static bool              s_done;       // psram_speed_boot ran
static bool              s_job;        // the trial's job task is running or done
static void            (*s_pass_cb)(void);
static bool            (*s_on_trial)(void); // ota_image_trial_maybe, from psram_speed_boot
static SemaphoreHandle_t s_mu;         // s_rec, s_trial_live, s_keep120 (never held across NVS or a restart)
static StaticSemaphore_t s_mu_buf;

static void lock(void) {
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
}

static void unlock(void) {
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

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

// The running image is on trial (PENDING_VERIFY): any restart now makes the bootloader roll it back. ota owns that state
// (#285) and sits above this guard, so psram_speed_boot is handed its reader. Not handed (yet): on trial, so nothing
// restarts for the speed.
static bool image_pending(void) {
    bool (*on_trial)(void) = s_on_trial;
    return on_trial ? on_trial() : true;
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

static bool rec_save(psram_speed_rec_t const *r) {
    nvs_handle_t nvs;
    bool         ok = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(nvs, REC_KEY, r, sizeof(*r)) == ESP_OK && nvs_commit(nvs) == ESP_OK;
        nvs_close(nvs);
    }
    if (!ok) {
        ESP_LOGE(TAG, "speed record not saved: 120 MHz is not armed, the next boot is at 80 MHz");
    }
    return ok;
}

// Saves a copy of s_rec outside the lock
static bool save_current(void) {
    lock();
    psram_speed_rec_t copy = s_rec;
    unlock();
    return rec_save(&copy);
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

// A factory reset restarts through the same paths and must come back at 80 (the record is erased with the settings)
void psram_speed_no_rearm(void) {
    s_keep120 = false;
    arm_register(false);
}

static void log_rec(char const *what) {
    char text[40];
    psram_speed_value_text(&s_rec, psram_speed_running(), text, sizeof(text));
    ESP_LOGI(TAG, "speed %s: running %u MHz, want %u, trial %u, failed %u (%s)", what, psram_speed_running(), s_rec.want,
             s_rec.trial, s_rec.failed, text);
}

// Trial set and saved first (H2: a record that did not save arms nothing), then the register, then one restart
static void arm_and_restart(void) {
    lock();
    psram_speed_rec_t copy = s_rec;
    unlock();
    bool saved = psram_speed_arm_commit(&copy, rec_save);
    lock();
    s_rec = copy;
    unlock();
    if (!saved) {
        ESP_LOGE(TAG, "speed: 120 MHz wanted but the record could not be saved: not armed, staying at 80 MHz");
        return;
    }
    ESP_LOGW(TAG, "speed: 120 MHz wanted, restarting once to boot at 120 MHz");
    arm_register(true);
    esp_restart(); // not system_restart: nothing to save, and a PSRAM trial must not start app save hooks
}

void psram_speed_boot(bool (*image_on_trial)(void)) {
    s_on_trial = image_on_trial; // before s_done: the guard task's tick reads it only once s_done is set
    s_done     = true;
    s_mu       = xSemaphoreCreateMutexStatic(&s_mu_buf);
    rec_load();
    uint32_t raw = psram_speed_carrier_raw();
    ESP_LOGI(TAG, "speed: carrier found at PSRAM init %08x, running %u MHz%s", (unsigned)raw, psram_speed_running(),
             psram_speed_supported() ? "" : " (IDF without the runtime speed patch)");
    psram_speed_rec_t before  = s_rec;
    bool              pending = image_pending();
    psram_speed_act_t act     = psram_speed_boot_decide(&s_rec, psram_speed_running(), psram_speed_supported(), pending, &s_trial_live);
    if (memcmp(&before, &s_rec, sizeof(s_rec)) != 0) {
        rec_save(&s_rec);
    }
    if (before.trial && !s_rec.trial && s_rec.failed) {
        ESP_LOGE(TAG, "a 120 MHz boot did not get through (crash, watchdog or restart before it passed): back at 80 MHz, "
                      "not tried again until chosen in Settings or with fw psram speed 120");
    }
    if (esp_register_shutdown_handler(shutdown_hook) != ESP_OK) {
        ESP_LOGE(TAG, "speed: no shutdown hook, a clean restart at 120 MHz comes back at 80 MHz");
    }
    log_rec(pending ? "record (image on trial: no restart for the speed until it is valid)" : "record");
    if (act == PSRAM_SPEED_ACT_ARM_RESTART) {
        arm_and_restart();
    } else if (act == PSRAM_SPEED_ACT_RESTART) {
        ESP_LOGW(TAG, "speed: running 120 MHz against the record, restarting at 80 MHz");
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

// A failed 120 MHz: the record says 80 and failed, nothing re-arms, the restart lands at 80. While the image is still on
// trial no restart (it would roll the image back): the record and the register say 80 and the normal validation decides;
// the tick restarts once the image is valid.
static void fail_and_restart(char const *why) {
    lock();
    s_keep120    = false;
    s_trial_live = false;
    psram_speed_fail(&s_rec);
    unlock();
    save_current();
    arm_register(false);
    if (image_pending()) {
        ESP_LOGE(TAG, "120 MHz failed (%s) on an image still on trial: no restart now, it goes back to 80 MHz once the image "
                      "is valid",
                 why);
        return;
    }
    ESP_LOGE(TAG, "120 MHz failed (%s): restarting at 80 MHz, not tried again until chosen", why);
    system_restart_note("psram 120 failed");
    esp_restart(); // not system_restart: no app save hooks with a PSRAM that is suspect
}

// The trial's own task (6 KB: the DMA gate needs more than the 3 KB guard task has): the automatic gate, then pass or fail
static char const JOB_ARM[]   = "@arm";   // sentinels, compared by address
static char const JOB_LEAVE[] = "@leave";

static void trial_job(void *arg) {
    char const *fail_why = arg;
    if (arg == JOB_ARM) {
        arm_and_restart(); // returns only when nothing was armed
        s_job = false;
        vTaskDelete(NULL);
        return;
    }
    if (arg == JOB_LEAVE) {
        arm_register(false);
        esp_restart();
    }
    if (!fail_why && !psram_dma_quick_gate()) {
        fail_why = "the DMA gate failed";
    }
    if (fail_why) {
        fail_and_restart(fail_why);
    } else {
        lock();
        s_trial_live = false;
        s_keep120    = true; // only a passed trial keeps 120 MHz through clean restarts
        psram_speed_pass(&s_rec);
        unlock();
        save_current();
        ESP_LOGI(TAG, "120 MHz trial passed (%d s, clean boot check, DMA gate round): 120 MHz stays", PSRAM_SPEED_TRIAL_SECONDS);
        if (s_pass_cb) {
            s_pass_cb();
        }
    }
    vTaskDelete(NULL);
}

static void start_job(char const *fail_why) {
    if (s_job) {
        return;
    }
    s_job = true;
    if (xTaskCreate(trial_job, "psram_trial", 6144, (void *)fail_why, 3, NULL) != pdPASS) {
        s_job = false; // retried by the next tick
        ESP_LOGE(TAG, "speed: no task for the 120 MHz trial result, retrying");
    }
}

void psram_speed_tick(void) {
    static unsigned n;
    if (s_trial_live) {
        char why[64];
        bool bad = psram_boot_bad(why, sizeof(why));
        switch (psram_speed_trial_step((unsigned)(esp_timer_get_time() / 1000000), bad, s_gate_failed)) {
            case PSRAM_SPEED_TRIAL_RUNNING: return;
            case PSRAM_SPEED_TRIAL_FAIL: start_job(bad ? "the boot check failed" : "the DMA gate failed"); return;
            case PSRAM_SPEED_TRIAL_PASS: start_job(NULL); return;
        }
        return;
    }
    if (!s_done || (++n % 10) != 0) { // once a second is enough for the checks below
        return;
    }
    // Waiting for the image to be valid (the OTA mark is later in app_main): then the deferred arm or the deferred leave
    psram_speed_rec_t r;
    lock();
    r = s_rec;
    unlock();
    bool pending = image_pending();
    if (psram_speed_arm_wanted(&r, psram_speed_running(), psram_speed_supported(), pending)) {
        start_job(JOB_ARM); // NVS and a restart: not on the 3 KB guard task
    } else if (psram_speed_leave_120_wanted(&r, psram_speed_running(), pending)) {
        ESP_LOGW(TAG, "speed: the image is valid now, leaving 120 MHz");
        start_job(JOB_LEAVE);
    }
}

void psram_speed_gate_result(bool failed) {
    if (!failed) {
        return;
    }
    s_gate_failed = true;
    if (psram_speed_running() == 120 && s_done) {
        console_reply("psram: the DMA gate failed at 120 MHz: back at 80 MHz");
        fail_and_restart("the DMA gate failed");
    }
}

bool psram_speed_choice(unsigned target) {
    if (!psram_speed_supported() || (target != 80 && target != 120) || image_pending()) {
        return false; // a restart now would roll the new image back
    }
    lock();
    psram_speed_rec_t copy = s_rec;
    unlock();
    psram_speed_choose(&copy, target);
    if (!rec_save(&copy)) { // saved first: RAM state changes only on success, and nothing is armed
        return false;
    }
    lock();
    s_rec        = copy;
    s_trial_live = false;
    s_keep120    = false; // the restart that follows is a 120 MHz one only through the register armed below
    unlock();
    arm_register(target == 120);
    ESP_LOGW(TAG, "speed: %u MHz chosen, the badge restarts", target);
    return true;
}

size_t psram_speed_value(char *out, size_t cap) {
    lock();
    psram_speed_rec_t r = s_rec;
    unlock();
    return psram_speed_value_text(&r, psram_speed_running(), out, cap);
}

unsigned psram_speed_other(void) {
    return s_rec.want == 120 && !s_rec.failed ? 80 : 120;
}

void psram_speed_print(void) {
    char text[40];
    lock();
    psram_speed_rec_t r = s_rec;
    bool              live = s_trial_live;
    unlock();
    psram_speed_value_text(&r, psram_speed_running(), text, sizeof(text));
    console_reply("psram speed: running %u MHz; %s; want %u, trial %s, failed %s; IDF %s; carrier found at PSRAM init %08x%s",
                  psram_speed_running(), text, r.want, live ? "running" : r.trial ? "armed" : "none", r.failed ? "yes" : "no",
                  psram_speed_supported() ? "runtime-selectable" : "NOT patched (0003)", (unsigned)psram_speed_carrier_raw(),
                  s_gate_failed ? "; DMA gate failed this boot" : "");
}

// "fw psram speed [80|120] [yes]": no argument = status; a speed without yes = what would happen
void psram_speed_cmd(char const *args) {
    unsigned          target = 0;
    psram_speed_cmd_t kind   = psram_speed_parse(args, &target);
    if (kind == PSRAM_SPEED_CMD_STATUS) {
        psram_speed_print();
        return;
    }
    if (!psram_speed_supported()) {
        console_reply("psram speed: this firmware was built with an IDF without patch 0003, 80 MHz only");
        return;
    }
    if (kind == PSRAM_SPEED_CMD_INVALID) {
        console_reply("psram speed [80|120] [yes]: 80 or 120 MHz, applied by a restart (yes to go on). If 120 MHz fails the "
                      "badge comes back at 80");
        return;
    }
    if (kind == PSRAM_SPEED_CMD_ASK) {
        console_reply("psram speed %u: this restarts the badge%s. Add yes to do it (fw psram speed %u yes)", target,
                      target == 120 ? "; if 120 MHz fails (boot check, crash, watchdog within 60 s, or a failed DMA gate) "
                                       "it comes back at 80 and stays there"
                                    : "",
                      target);
        return;
    }
    if (image_pending()) {
        console_reply("psram speed: this image is still on trial (not yet marked valid); a restart now would roll it back. "
                      "Try again in a minute");
        return;
    }
    if (!psram_speed_choice(target)) {
        console_reply("psram speed: not saved, nothing changed");
        return;
    }
    console_reply("psram speed %u: saved, restarting now", target);
    system_restart("psram speed");
}
