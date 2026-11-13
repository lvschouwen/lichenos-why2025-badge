#include "boot_progress.h"

#include "boot_keys.h"

#include "fw_console.h"
#include "fw_store.h"
#include "log_tap.h"

#include "esp_timer.h"

#include <stdio.h>
#include <strings.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

void boot_progress_begin(boot_step_t step) {
    portENTER_CRITICAL(&lock);
    boot_step_begin(step);
    portEXIT_CRITICAL(&lock);
}

void boot_progress_begin_background(boot_step_t step) {
    portENTER_CRITICAL(&lock);
    boot_step_begin_background(step);
    portEXIT_CRITICAL(&lock);
}

void boot_progress_progress(boot_step_t step, int pct) {
    portENTER_CRITICAL(&lock);
    boot_step_progress(step, pct);
    portEXIT_CRITICAL(&lock);
}

// "boot fail <step>": the next boot marks that step failed, once, so the failure view can be seen on the bench. The
// flag lives in NVS: main.c calls boot_progress_nvs_up() right after nvs_flash_init, which reads and clears it.
static int8_t       fail_step = -1;
static fw_get_u8_fn s_get; // the settings service (#245): lichen_boot opens no settings of its own
static fw_set_u8_fn s_set;

void boot_progress_bind_store(uint8_t (*get)(char const *key, uint8_t def),
                              bool (*set)(char const *key, uint8_t value)) {
    s_get = get;
    s_set = set;
}

static uint8_t flag_get(void) {
    return s_get ? s_get("boot_fail", 0) : 0;
}

static bool flag_set(uint8_t v) {
    return s_set && s_set("boot_fail", v);
}

void boot_progress_nvs_up(void) {
    uint8_t flag = flag_get();
    if (flag) {
        flag_set(0);
        fail_step = (int8_t)boot_fail_step(flag);
    }
}

void boot_progress_end(boot_step_t step, step_state_t state, char const *reason) {
    if (step == fail_step && state == STEP_DONE) {
        state = STEP_FAILED;
        reason = "simulated (fw boot fail)";
    }
    portENTER_CRITICAL(&lock);
    boot_step_end(step, state, reason);
    portEXIT_CRITICAL(&lock);
}

void boot_progress_view(boot_view_t *out) {
    portENTER_CRITICAL(&lock);
    boot_steps_view(out);
    portEXIT_CRITICAL(&lock);
}

char const *boot_progress_label(boot_step_t step) {
    return boot_step_label(step); // constant table, no lock needed
}

static void cmd_boot(char const *args) {
    if (strncmp(args, "keys", 4) == 0 && (args[4] == '\0' || args[4] == ' ')) {
        boot_keys_cmd(args + 4); // dev-gated there (#379)
        return;
    }
    if (strncmp(args, "fail ", 5) == 0) {
        for (int i = 0; i < BOOT_STEP_COUNT; i++) {
            if (boot_step_failable((boot_step_t)i) && strcasecmp(args + 5, boot_step_label((boot_step_t)i)) == 0) {
                flag_set((uint8_t)(i + 1));
                console_reply("boot: %s will fail on the next boot (once)", args + 5);
                return;
            }
        }
        console_reply("boot fail <storage|display|C6|Wi-Fi|clock|LoRa|apps>: unknown step");
        return;
    }
    static char const *const state_text[] = {"pending", "active", "done", "failed", "skipped"};
    boot_view_t              view;
    boot_progress_view(&view);
    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        char when[32] = "";
        if (view.began[i] && view.ended[i]) {
            snprintf(when, sizeof(when), "%u-%u ms", (unsigned)view.begin_ms[i], (unsigned)view.end_ms[i]);
        } else if (view.began[i]) {
            snprintf(when, sizeof(when), "%u- ms", (unsigned)view.begin_ms[i]);
        } else if (view.ended[i]) {
            snprintf(when, sizeof(when), "at %u ms", (unsigned)view.end_ms[i]); // ended without a begin
        }
        console_reply("boot %-7s %-8s %-14s %s", boot_progress_label((boot_step_t)i), state_text[view.state[i]], when,
                      view.reason[i]);
    }
    console_reply("boot current %s pct %d generation %u", boot_progress_label(view.current), view.pct, (unsigned)view.generation);
    static char lines[LOG_TAP_LINES][LOG_TAP_LINE_LEN]; // console task only
    size_t      n = log_tap_last(lines, LOG_TAP_LINES);
    for (size_t i = 0; i < n; i++) {
        console_reply("log %s", lines[i]);
    }
}

static uint32_t uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void boot_progress_start(void) {
    boot_steps_set_clock(uptime_ms); // from reset: the systimer counts the ROM and bootloader too (to be checked, #39)
    portENTER_CRITICAL(&lock);
    boot_steps_reset();
    portEXIT_CRITICAL(&lock);
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    console_register("boot", "boot [fail <step> | keys <down|esc+bksp|none>] (start-up steps and the last log lines; fail marks a step failed on the next boot; keys holds a combo at the next software restart, once, developer mode)", cmd_boot);
#else // a release names no debug subcommand (build.sh release_checks)
    console_register("boot", "boot [fail <step>] (start-up steps and the last log lines; fail marks a step failed on the next boot)", cmd_boot);
#endif
}
