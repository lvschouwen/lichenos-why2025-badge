#include "psram_speed.h"

#include "psram_boot.h"
#include "psram_dma_cmd.h"
#include "psram_nvs.h"
#include "psram_speed_logic.h"
#include "psram_tuning.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
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

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if CONFIG_SPIRAM_SPEED_RUNTIME_SELECT
#include "esp_private/mspi_timing_tuning.h"
#endif

#define REC_KEY  "psram_spd"
#define CARRIER  LP_SYSTEM_REG_LP_STORE15_REG // spike #121: survives esp_restart, ota-reboot and an int-WDT reset

static char const TAG[] = "psram";

// psram_speed_logic.c names and classifies reset reasons by their esp_reset_reason_t number
_Static_assert(ESP_RST_SW == 3 && ESP_RST_PANIC == 4 && ESP_RST_INT_WDT == 5 && ESP_RST_TASK_WDT == 6 && ESP_RST_WDT == 7 &&
                   ESP_RST_BROWNOUT == 9 && ESP_RST_CPU_LOCKUP == 15,
               "esp_reset_reason_t order changed: update psram_speed_reset_name / psram_speed_reset_crashed");

static psram_speed_rec_t s_rec = {.want = 120};
static bool              s_keep120;      // a clean restart arms 120 MHz (the shutdown hook). RAM only: a crash comes back at 80
static bool              s_arm_deferred; // an arm restart waits for the image to be valid (the tick starts it)
static _Atomic uint8_t   s_evidence;     // psram_fail_kind_t seen this boot and not yet judged (the tick judges it)
static bool              s_done;         // psram_speed_boot ran
static bool              s_job;          // the speed job task is running
static psram_speed_hooks_t s_hooks;     // from psram_speed_boot: the image's trial state, the crash's record
static bool              s_rearm_wait;   // a settled session crashed: re-arm 120 once the crash is recorded and told
static uint32_t          s_idle_word;    // what STORE15 holds when not armed: 0, or a fall that could not be saved
typedef struct {
    uint32_t last_us, max_us; // the last and the longest PSRAM heap check
    unsigned checks;
    unsigned guard_free;      // the guard task's stack high-water after a heap check, bytes
} heap_stats_t;
static heap_stats_t s_heap; // written by the guard task, read by fw psram speed: under s_heap_mux, copied whole
static portMUX_TYPE s_heap_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_mu;           // s_rec, s_keep120 (never held across NVS or a restart)
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

unsigned psram_speed_want(void) {
    return s_rec.want;
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
    bool (*on_trial)(void) = s_hooks.image_on_trial;
    return on_trial ? on_trial() : true;
}

// The record goes straight to NVS, not through the settings service (it runs before the services); psram_nvs.c
// reports a failed write (#325). One blob, the layout of psram_speed_rec_encode; anything else reads as the default record.
static void rec_load(void) {
    nvs_handle_t nvs;
    uint8_t      blob[PSRAM_SPEED_REC_SIZE];
    size_t       size = 0; // nothing read = the default record
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size = sizeof(blob);
        if (nvs_get_blob(nvs, REC_KEY, blob, &size) != ESP_OK) { // a longer blob is not ours either
            size = 0;
        }
        nvs_close(nvs);
    }
    psram_speed_rec_decode(blob, size, &s_rec);
}

static bool rec_save(psram_speed_rec_t const *r) {
    nvs_handle_t nvs;
    uint8_t      blob[PSRAM_SPEED_REC_SIZE];
    psram_speed_rec_encode(r, blob);
    bool ok = psram_nvs_open(REC_KEY, &nvs) == ESP_OK; // a failure is logged and counted there (#325)
    if (ok) {
        ok = psram_nvs_put(nvs, REC_KEY, blob, sizeof(blob)) == ESP_OK;
        nvs_close(nvs);
    }
    return ok; // the caller says what a lost record means where it is
}

// Twice: a fall must not get lost to one refused write
static bool rec_save_retry(psram_speed_rec_t const *r) {
    return rec_save(r) || rec_save(r);
}

// Saves a copy of s_rec outside the lock
static bool save_current(void) {
    lock();
    psram_speed_rec_t copy = s_rec;
    unlock();
    return rec_save(&copy);
}

static void arm_register(bool on) {
    REG_WRITE(CARRIER, on ? PSRAM_SPEED_ARM_WORD : s_idle_word);
}

// esp_restart() and everything built on it (system_restart, update, power menu, fw reboot, a bare esp_restart) run this;
// a panic, WDT or brownout does not, which is what makes those come back at 80 (then arm on the next clean restart).
static void shutdown_hook(void) {
    if (s_keep120) {
        arm_register(true);
    }
}

// A factory reset restarts through the same paths and must come back at 80 (the record is erased with the settings)
void psram_speed_no_rearm(void) {
    s_keep120      = false;
    s_rearm_wait   = false;
    s_arm_deferred = false;
    s_idle_word    = 0;
    arm_register(false);
}

// The facts of a judgement made now, on this boot
static psram_fail_ctx_t fail_ctx(void) {
    time_t now = time(NULL);
    return (psram_fail_ctx_t){.reset    = (uint8_t)esp_reset_reason(),
                              .carried  = (uint8_t)psram_speed_carried_fall(psram_speed_carrier_raw()),
                              .wall_s   = now > 0 && now <= (time_t)UINT32_MAX ? (uint32_t)now : 0,
                              .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
                              .fw       = esp_app_get_description()->version};
}

static void log_rec(char const *what) {
    char text[40];
    psram_speed_value_text(&s_rec, psram_speed_running(), text, sizeof(text));
    ESP_LOGI(TAG, "speed %s: running %u MHz, want %u, failed %u, arms %u (%s)", what, psram_speed_running(), s_rec.want,
             s_rec.failed, s_rec.arms, text);
}

// The arm counted and saved first (H2: a record that did not save arms nothing), then the register, then one restart.
// late_why: from the job task once the image is valid, with the apps running: through system_restart and their saves
// (the notices among them); NULL early in the boot: nothing to save yet, and no trial gate to wait for
static void arm_and_restart(char const *late_why) {
    lock();
    psram_speed_rec_t copy = s_rec;
    unlock();
    bool saved = psram_speed_arm_commit(&copy, rec_save);
    lock();
    s_rec = copy;
    unlock();
    if (!saved) {
        s_keep120 = false; // nothing may arm a record that is not on the flash
        ESP_LOGE(TAG, "speed: 120 MHz wanted but the record could not be saved: not armed, staying at 80 MHz");
        return;
    }
    ESP_LOGI(TAG, "speed: 120 MHz wanted, restarting once to boot at 120 MHz");
    arm_register(true);
    if (late_why) {
        system_restart(late_why); // the shutdown hook arms again (s_keep120): the saves cannot undo it
    } else {
        esp_restart();
    }
}

// Leaving 120 against the record: the register clear, a bare restart (as the fall: PSRAM may be suspect), noted
static void leave_restart(void) {
    arm_register(false);
    system_restart_note("psram leave 120");
    esp_restart();
}

// The fall to 80 is a feature lost until someone acts: E (#113), on the boot that falls and on every boot after it
static void log_fell(bool now) {
    char text[112];
    psram_speed_fail_text(&s_rec.last, text, sizeof(text));
    ESP_LOGE(TAG, "120 MHz %s on PSRAM evidence (%s): PSRAM stays at 80 MHz until 120 MHz is chosen in Settings or "
                  "with fw psram speed 120 yes",
             now ? "failed" : "failed earlier", text);
}

// Back to 80 after a fall (failed set): the register clear, one restart. A fall that could not be saved goes in STORE15
// instead (any word but the ARM word selects 80), and the next boot records it and arms nothing (review L2). While the
// image is still on trial no restart (it would roll the image back): the tick leaves 120 once the image is valid.
static void fall_restart(bool saved, psram_fail_kind_t kind) {
    if (!saved) {
        s_idle_word = psram_speed_fall_word(kind);
        ESP_LOGE(TAG, "speed: the fall to 80 MHz could not be saved: carried in the LP register, the next boot records it "
                      "and does not arm 120 MHz");
    }
    arm_register(false);
    if (image_pending()) {
        ESP_LOGE(TAG, "speed: the image is still on trial: no restart now, 80 MHz once the image is valid");
        return;
    }
    system_restart_note("psram 120 failed");
    esp_restart(); // not system_restart: no app save hooks with a PSRAM that is suspect
}

// Bad bytes in this boot's dense check are evidence; a fallen-back or short tuning window alone is not (#353)
static psram_fail_kind_t boot_evidence(void) {
    return psram_speed_running() == 120 && psram_boot_bad_bytes() ? PSRAM_FAIL_BOOT_CHECK : PSRAM_FAIL_NONE;
}

static void boot_act(psram_speed_act_t act, bool pending, bool crashed, bool saved) {
    switch (act) {
    case PSRAM_SPEED_ACT_ARM_RESTART:
        s_keep120 = true; // until the arm: a clean restart meanwhile goes to 120 as well
        if (pending) {
            s_arm_deferred = true;
        } else {
            arm_and_restart(NULL);
        }
        return;
    case PSRAM_SPEED_ACT_KEEP120:
        s_keep120 = true;
        if (crashed) { // unexpected but handled: W (#113)
            ESP_LOGW(TAG, "speed: 120 MHz wanted, this boot stays at 80 MHz after a crash (no evidence against 120); the "
                          "next restart goes to 120 MHz");
        } else {
            ESP_LOGI(TAG, "speed: 120 MHz armed %d times in a row without settling: this boot stays at 80 MHz, the next "
                          "restart goes to 120 MHz",
                     PSRAM_SPEED_MAX_ARMS);
        }
        return;
    case PSRAM_SPEED_ACT_REARM_AFTER_CRASH:
        s_keep120    = true;
        s_rearm_wait = true; // the tick re-arms once the crash log has it and its notice is raised (Lucas, #353)
        ESP_LOGW(TAG, "speed: 120 MHz wanted, this boot is at 80 MHz after a crash (no evidence against 120): 120 MHz "
                      "again once the crash is recorded");
        return;
    case PSRAM_SPEED_ACT_LEAVE120:
        if (!pending) {
            ESP_LOGW(TAG, "speed: running 120 MHz against the record, restarting at 80 MHz");
            leave_restart();
        }
        return;
    case PSRAM_SPEED_ACT_FALL80:
        log_fell(true);
        fall_restart(saved, (psram_fail_kind_t)s_rec.last.kind);
        return;
    case PSRAM_SPEED_ACT_NONE:
    default:
        s_keep120 = psram_speed_supported() && psram_speed_running() == 120 && s_rec.want == 120 && !s_rec.failed;
        return;
    }
}

void psram_speed_boot(psram_speed_hooks_t const *hooks) {
    s_hooks    = hooks ? *hooks : (psram_speed_hooks_t){0};
    s_mu       = xSemaphoreCreateMutexStatic(&s_mu_buf);
    rec_load();
    ESP_LOGI(TAG, "speed: carrier found at PSRAM init %08x, running %u MHz%s", (unsigned)psram_speed_carrier_raw(),
             psram_speed_running(), psram_speed_supported() ? "" : " (IDF without the runtime speed patch)");
    psram_speed_rec_t before  = s_rec;
    bool              pending = image_pending();
    psram_fail_ctx_t  ctx     = fail_ctx();
    ctx.fresh_dump = s_hooks.dump_fresh && s_hooks.dump_fresh(); // reason 7 is a crash only with a new dump (crash.c)
    bool              crashed = psram_speed_reset_crashed(ctx.reset, ctx.fresh_dump);
    psram_speed_act_t act = psram_speed_boot_decide(&s_rec, psram_speed_running(), psram_speed_supported(), boot_evidence(), &ctx);
    bool saved = true;
    if (memcmp(&before, &s_rec, sizeof(s_rec)) != 0) {
        saved = rec_save_retry(&s_rec); // a fall (this boot's or a carried one), or the arm cap starting over
        if (!saved && act != PSRAM_SPEED_ACT_FALL80) {
            ESP_LOGE(TAG, "speed record not saved at boot: the next boot makes the same judgement");
        }
    }
    if (esp_register_shutdown_handler(shutdown_hook) != ESP_OK) {
        ESP_LOGE(TAG, "speed: no shutdown hook, a clean restart at 120 MHz comes back at 80 MHz");
    }
    log_rec(pending ? "record (image on trial: no restart for the speed until it is valid)" : "record");
    if (before.failed && s_rec.failed && psram_speed_supported()) {
        log_fell(false);
    }
    if (ctx.carried != PSRAM_FAIL_NONE && s_rec.failed) {
        s_idle_word = saved ? 0 : psram_speed_fall_word((psram_fail_kind_t)ctx.carried); // still not saved: carry it on
        arm_register(false);
        log_fell(true);
    }
    boot_act(act, pending, crashed, saved);
    s_done = true; // after the decision: the tick reads s_rec and the flags only from here (review L3)
}

// PSRAM evidence seen later in this boot: the record says 80 and failed, nothing re-arms, the restart lands at 80
static void fall_now(psram_fail_kind_t kind) {
    psram_fail_ctx_t ctx = fail_ctx();
    lock();
    psram_speed_act_t act = psram_speed_evidence(&s_rec, psram_speed_running(), kind, &ctx);
    if (act == PSRAM_SPEED_ACT_FALL80) {
        s_keep120 = false;
    }
    unlock();
    if (act != PSRAM_SPEED_ACT_FALL80) {
        return; // at 80, or it already fell
    }
    lock();
    psram_speed_rec_t copy = s_rec;
    unlock();
    bool saved = rec_save_retry(&copy);
    log_fell(true);
    fall_restart(saved, kind);
}

// The speed's own task (NVS and a restart: not on the 3 KB guard task): a fall, the deferred arm or leave, the settle
enum { JOB_ARM = 0x100, JOB_REARM, JOB_LEAVE, JOB_SETTLE }; // anything below is a psram_fail_kind_t

static void speed_job(void *arg) {
    int job = (int)(intptr_t)arg;
    if (job == JOB_ARM) {
        arm_and_restart("psram 120 arm"); // returns only when nothing was armed
    } else if (job == JOB_REARM) {
        arm_and_restart("psram 120 re-arm after crash");
    } else if (job == JOB_LEAVE) {
        ESP_LOGW(TAG, "speed: the image is valid now, leaving 120 MHz");
        leave_restart();
    } else if (job == JOB_SETTLE) {
        lock();
        s_rec.arms = 0;
        unlock();
        save_current(); // a failed save only keeps the count: the cap ends an arm loop, it never fails 120
    } else {
        fall_now((psram_fail_kind_t)job);
    }
    s_job = false;
    vTaskDelete(NULL);
}

// False when it did not start: the caller keeps what it wanted for the next tick
static bool start_job(int job) {
    if (s_job) {
        return false;
    }
    s_job = true;
    if (xTaskCreate(speed_job, "psram_speed", 6144, (void *)(intptr_t)job, 3, NULL) != pdPASS) {
        s_job = false;
        ESP_LOGW(TAG, "speed: no task for the speed job, retrying");
        return false;
    }
    return true;
}

void psram_speed_report(psram_fail_kind_t kind) {
    if (psram_speed_running() != 120) {
        return; // at 80 there is nothing to fall to: the finding is logged where it was made
    }
    uint8_t none = PSRAM_FAIL_NONE;
    atomic_compare_exchange_strong(&s_evidence, &none, (uint8_t)kind); // the first evidence is the one recorded
}

// The PSRAM heap's own check (CORRUPT HEAP) at 120 MHz: a walk of every block under the heap's lock, like the stress
// run's. Its time and the guard task's stack are kept for fw psram speed (review M2: judged on the bench)
#ifndef PSRAM_SPEED_HEAP_CHECK_S
#define PSRAM_SPEED_HEAP_CHECK_S 60 // seconds between checks; 0 = off
#endif

static void heap_check(psram_speed_rec_t const *r) {
    static unsigned n;
    unsigned const  period = PSRAM_SPEED_HEAP_CHECK_S; // a variable, so 0 (off) does not trip -Wtype-limits
    if (period == 0 || psram_speed_running() != 120 || r->want != 120 || r->failed || ++n % period != 0) {
        return;
    }
    int64_t  t0 = esp_timer_get_time();
    bool     ok = heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (!ok) {
        ESP_LOGE(TAG, "the PSRAM heap failed its integrity check at 120 MHz");
        psram_speed_report(PSRAM_FAIL_HEAP);
    }
    unsigned free_b = (unsigned)uxTaskGetStackHighWaterMark(NULL); // after the failure line too: the guard task's stack
    portENTER_CRITICAL(&s_heap_mux);
    bool longest     = us > s_heap.max_us;
    s_heap.last_us   = us;
    s_heap.max_us    = longest ? us : s_heap.max_us;
    s_heap.guard_free = free_b;
    s_heap.checks++;
    portEXIT_CRITICAL(&s_heap_mux);
    if (longest) {
        ESP_LOGI(TAG, "speed: PSRAM heap check took %u us (longest so far), guard stack %u B never used", (unsigned)us,
                 free_b);
    }
}

// The re-arm after a settled session's crash: once the crash log has the record and its notice is raised (the restart
// saves the notices), never on an image on trial
static bool crash_told(void) {
    return s_hooks.crash_told && s_hooks.crash_told();
}

void psram_speed_tick(void) {
    static unsigned n;
    if (!s_done || (++n % 10) != 0) { // once a second is enough
        return;
    }
    psram_speed_rec_t r;
    lock();
    r = s_rec;
    unlock();
    heap_check(&r);
    uint8_t  evidence = atomic_load(&s_evidence);
    bool     pending  = image_pending();
    unsigned up       = (unsigned)(esp_timer_get_time() / 1000000);
    if (evidence != PSRAM_FAIL_NONE) {
        if (start_job(evidence)) {
            atomic_store(&s_evidence, PSRAM_FAIL_NONE);
        }
    } else if (pending) {
        return; // a restart now would roll the image back: the arm and the leave wait for the valid mark
    } else if (s_arm_deferred) {
        s_arm_deferred = !start_job(JOB_ARM);
    } else if (psram_speed_rearm_now(s_rearm_wait, crash_told(), pending)) {
        s_rearm_wait = !start_job(JOB_REARM);
    } else if (psram_speed_leave_120_wanted(&r, psram_speed_running())) {
        start_job(JOB_LEAVE);
    } else if (psram_speed_settle_wanted(&r, psram_speed_running(), up)) {
        start_job(JOB_SETTLE);
    }
}

void psram_speed_gate_result(bool bad_bytes, bool console) {
    if (!bad_bytes || psram_speed_running() != 120 || !s_done) {
        return;
    }
    if (console) {
        console_reply("psram: the DMA gate found bad PSRAM bytes at 120 MHz: back at 80 MHz");
    }
    psram_speed_report(PSRAM_FAIL_DMA_GATE);
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
    s_rec          = copy;
    s_keep120      = false; // the restart that follows is a 120 MHz one only through the register armed below
    s_arm_deferred = false;
    s_rearm_wait   = false;
    s_idle_word    = 0;     // a carried fall is history once a speed is chosen
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
    unlock();
    psram_speed_value_text(&r, psram_speed_running(), text, sizeof(text));
    console_reply("psram speed: running %u MHz; %s; want %u, failed %s, arms %u of %u; IDF %s; carrier found at PSRAM init %08x",
                  psram_speed_running(), text, r.want, r.failed ? "yes" : "no", r.arms, PSRAM_SPEED_MAX_ARMS,
                  psram_speed_supported() ? "runtime-selectable" : "NOT patched (0003)", (unsigned)psram_speed_carrier_raw());
    char last[112];
    psram_speed_fail_text(&r.last, last, sizeof(last));
    console_reply("psram speed: last fall to 80: %s", last);
    portENTER_CRITICAL(&s_heap_mux);
    heap_stats_t h = s_heap;
    portEXIT_CRITICAL(&s_heap_mux);
    console_reply("psram speed: heap check every %d s at 120 MHz: %u done, last %u us, longest %u us; guard stack %u B "
                  "never used%s",
                  PSRAM_SPEED_HEAP_CHECK_S, h.checks, (unsigned)h.last_us, (unsigned)h.max_us, h.guard_free,
                  s_rearm_wait ? "; 120 MHz re-arm waits for the crash record" : "");
}

bool psram_speed_fell(char *why, size_t cap) {
    lock();
    psram_speed_rec_t r = s_rec;
    unlock();
    bool fell = psram_speed_supported() && r.failed;
    if (cap) {
        snprintf(why, cap, fell ? "PSRAM runs at 80 MHz after a %s at 120 MHz. Settings > PSRAM speed sets 120 again." : "",
                 psram_speed_fail_kind_text(r.last.kind));
    }
    return fell;
}

void psram_speed_record(psram_speed_rec_t *out) {
    lock();
    *out = s_rec;
    unlock();
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
        console_reply("psram speed [80|120] [yes]: 80 or 120 MHz, applied by a restart (yes to go on). If the PSRAM shows "
                      "errors at 120 MHz the badge comes back at 80");
        return;
    }
    if (kind == PSRAM_SPEED_CMD_ASK) {
        console_reply("psram speed %u: this restarts the badge%s. Add yes to do it (fw psram speed %u yes)", target,
                      target == 120 ? "; if the PSRAM shows errors at 120 MHz (boot check, canary, heap check, DMA gate) "
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
    // Gated (#304): refused above on trial, so no wait; if it did wait (up to OTA_RESTART_GATE_MAX_S of uptime), the
    // console's run_mutex and a Wi-Fi console's reply would be held that long
    system_restart("psram speed");
}
