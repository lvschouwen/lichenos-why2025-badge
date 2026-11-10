// The thermal watchdog (#10). The IP5306 charger has no data link to the MCUs, so the badge cannot see its two
// unprotected cells; heat is the one thing it can notice. Every 5 s a task on core 0 reads the sensors service's
// snapshot (the BME690 beside the cells, the P4 die sensor), asks thermal_rules.c what it means and acts:
//   warn      status bar HOT, a notice, a full-screen warning once, load shed (backlight to the minimum, Wi-Fi and LoRa off)
//   shutdown  a notice (saved by the power task), then Power off (#65): C6 held in reset, P4 in deep sleep, the IP5306
//             cuts the cells after ~32 s
// The board source is the BME690's raw temperature, uncorrected: with the gas heater on it reads a few K above the
// board (warns early, the safe side); the die sensor has limits of its own. The C6's
// temperature is not on the C6 link, so it is not a source.
// Also here: the one-time "your cells are unprotected" notice, and `fw thermal` with a simulation (`test`) that feeds a
// board reading through the same decision path.
// An OS service since #281: main starts it in its own boot step, display or not (lichen_watch_start), and what it needs
// from the shell (the warning page, the Power app's goodbye path, "home is shown") comes in as hooks.
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_os.h"
#include "lichen_sys_display.h"
#include "lichen_sys_power.h"
#include "lichen_sys_radio.h"
#include "lichen_sys_thermal.h"
#include "fw_console.h"
#include "lichen_notify.h"
#include "lichen_task.h"
#include "lichen_radio.h"
#include "lichen_sensors.h"
#include "sdkconfig.h"
#include "settings_store.h"
#include "thermal_rules.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "thermal";

#define POLL_MS        5000
#define STACK_BYTES    4096
#define STALE_US       (30 * 1000000LL) // a sensor snapshot older than this is no reading
#define SETTLE_POLLS   3                // after home shows (the first-run notice waits for it)
#define SIM_WARN_C     55.0f
#define SIM_SHUTDOWN_C 65.0f
#define SIM_MAX_S      120              // a forgotten simulation ends by itself
#define NO_SIM         INT32_MIN
#define EVENT_LOG      6
#define SEEN_KEY       "batt_seen"
#define OFF_BOUND_MS   30000 // the shutdown's backstop: the badge is off by then, saved or not
#define BACKSTOP_STACK 4096
#define SHED_KEY       "thermal_shed" // written by firmware before #282 while the radios were shed (and saved off)

#if CONFIG_LICHEN_SENSORS_GAS_HEATER
#define HEATER_ON true
#else
#define HEATER_ON false
#endif

typedef struct {
    uint32_t up_s;
    char     text[48];
} event_t;

static portMUX_TYPE   lock = portMUX_INITIALIZER_UNLOCKED;
static lichen_sys_thermal_info_t info; // under lock
static event_t        events[EVENT_LOG];
static unsigned       event_count; // under lock
// Simulated board temperature: generation in the high 32 bits, milli-degrees (or NO_SIM) in the low 32. One word, so a
// reading and the generation it belongs to are always read together.
static _Atomic uint64_t sim_state = (uint32_t)NO_SIM;
#define SIM_MC(v)  ((int32_t)(uint32_t)(v))
#define SIM_GEN(v) ((uint32_t)((v) >> 32))
static _Atomic uint32_t sim_since_s;
static atomic_bool    shutdown_asked;
static lichen_watch_hooks_t hooks;      // set once before the task starts
static atomic_bool          running;    // the task is up (`fw thermal` says so, #281)
static _Atomic uint32_t     polls;      // polls so far
static _Atomic uint32_t     last_poll_s; // uptime of the last one

static uint32_t up_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void log_event(char const *text) {
    taskENTER_CRITICAL(&lock);
    event_t *e = &events[event_count % EVENT_LOG];
    e->up_s    = up_s();
    snprintf(e->text, sizeof(e->text), "%s", text);
    event_count++;
    taskEXIT_CRITICAL(&lock);
}

void lichen_sys_thermal_info(lichen_sys_thermal_info_t *out) {
    taskENTER_CRITICAL(&lock);
    *out = info;
    taskEXIT_CRITICAL(&lock);
}

// ---- load shedding ---------------------------------------------------------------------------------------------------

// Everything that sheds load while the badge is warm, in one place. Backlight to the minimum (not saved, so the
// person's levels come back) and Wi-Fi and LoRa held off by the radio service (#282): a hold is never saved, so the
// person's switches stay as they were, a reboot while warm brings the radios back as set, and on a clear they come back
// the same way (a radio the person switched off meanwhile stays off, one switched on meanwhile already is).
static void shed_load(bool on) {
    static uint8_t saved[2];
    static bool    shed;
    if (on == shed) {
        return;
    }
    shed = on;
    if (on) {
        lichen_sys_display_get(&saved[0], &saved[1]);
        lichen_sys_display_set(LICHEN_SYS_DISPLAY_MIN, 0);
        lichen_sys_radio_hold(RADIO_HOLD_THERMAL, (uint8_t)(1u << RADIO_WIFI | 1u << RADIO_LORA));
    } else {
        lichen_sys_display_set(saved[0], saved[1]);
        lichen_sys_radio_release(RADIO_HOLD_THERMAL);
    }
}

// ---- reacting --------------------------------------------------------------------------------------------------------

static void say(ln_level_t level, char const *title, char const *body, uint32_t flags) {
    // The log line has the notice's level (#113)
    esp_log_level_t log = level == LN_ERROR ? ESP_LOG_ERROR : level == LN_WARN ? ESP_LOG_WARN : ESP_LOG_INFO;
    ESP_LOG_LEVEL(log, TAG, "%s: %s", title, body);
    log_event(title);
    lichen_notify_to("lichen", level, "battery", title, body, flags);
}

static void cause_text(lichen_sys_thermal_info_t const *i, char *out, size_t cap) {
    snprintf(out, cap, "%s %s at %.0f \xC2\xB0""C.", th_src_name(i->cause), i->cause_rise ? "rose fast, now" : "reached",
             (double)i->cause_c);
}

// The shell's goodbye path when it runs, main's headless one otherwise; with no hook at all the badge still goes down
static void power_off(void) {
    if (hooks.power_off) {
        hooks.power_off();
    } else {
        ESP_LOGE(TAG, "no power-off hook: switching off without the saves");
        lichen_sys_power_off_now();
    }
}

// The backstop (#281): whatever the power-off path waits on (an app that does not stop, a stuck card, the LVGL lock),
// the badge is off OFF_BOUND_MS after the shutdown was decided. Its own task, so nothing the path holds can stop it.
static void backstop_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OFF_BOUND_MS));
    ESP_LOGE(TAG, "power off did not finish in %d s: switching off now", OFF_BOUND_MS / 1000);
    lichen_sys_power_off_now();
}

// The thermal shutdown: the backstop first, then the notice (the power path saves it), then the power off. No load
// shed before it: holding the radios waits on the radio service's change lock and the listeners (the C6), and the
// shutdown must not wait behind them while the cells heat.
static void shut_down(lichen_sys_thermal_info_t const *now) {
    if (atomic_exchange(&shutdown_asked, true)) {
        return;
    }
    if (!lichen_task_start_in(NULL, "thoff", backstop_task, NULL, BACKSTOP_STACK, LICHEN_PRIO_URGENT,
                              LICHEN_STACK_LP_RAM, NULL)) { // never returns (deep sleep), so LP RAM is fine (#269)
        ESP_LOGE(TAG, "no memory for the power-off backstop");
    }
    char body[120];
    cause_text(now, body, sizeof(body));
    size_t n = strlen(body);
    snprintf(body + n, sizeof(body) - n, " Powering off to protect the cells.");
    say(LN_ERROR, "Too hot: powering off", body, 0);
    power_off();
}

static void on_events(unsigned ev) {
    lichen_sys_thermal_info_t now;
    lichen_sys_thermal_info(&now);
    char body[120];
    if (ev & TH_EV_SHUTDOWN) {
        shut_down(&now); // nothing else first: no load shed, no warning page (either can wait on the C6 or the UI)
        return;
    }
    if (ev & TH_EV_WARN) {
        cause_text(&now, body, sizeof(body));
        size_t n = strlen(body);
        snprintf(body + n, sizeof(body) - n, " Screen dimmed, radios off. Unplug the charger and let it cool.");
        say(LN_WARN, "Badge is getting hot", body, LN_COALESCE);
        shed_load(true);
        if (hooks.hot_alert) {
            hooks.hot_alert();
        }
    }
    if (ev & TH_EV_CLEARED) {
        say(LN_INFO, "Badge has cooled down", "Back to normal.", LN_COALESCE);
        shed_load(false);
    }
    if (ev & TH_EV_BLIND) {
        say(LN_WARN, "Temperature not watched",
            "No temperature sensor answers, so the badge cannot warn you about heat. Keep an eye on it.", LN_COALESCE);
    }
    if (ev & TH_EV_BOARD_LOST) {
        say(LN_WARN, "Battery-side sensor lost",
            "The air sensor beside the batteries stopped answering. Only the chip temperature is watched now, "
            "which says little about the cells.", LN_COALESCE);
    }
    if (ev & TH_EV_BOARD_BACK) {
        say(LN_INFO, "Battery-side sensor is back", "The air sensor answers again.", LN_COALESCE);
    }
    if (ev & TH_EV_SIGHT) {
        say(LN_INFO, "Temperature watch is back", "A sensor answers again.", LN_COALESCE);
    }
}

// ---- reading ---------------------------------------------------------------------------------------------------------

// Swaps the simulation (NO_SIM = off) under a new generation. Switching off with nothing running changes nothing, so
// the real history is kept. Returns whether anything changed.
static bool sim_write(int32_t mc, bool start) {
    uint64_t old = atomic_load(&sim_state);
    do {
        if (!start && SIM_MC(old) == NO_SIM) {
            return false;
        }
    } while (!atomic_compare_exchange_weak(&sim_state, &old, ((uint64_t)(SIM_GEN(old) + 1) << 32) | (uint32_t)mc));
    return true;
}

// Returns the simulation generation the board reading was taken under
static uint32_t read_sources(th_input_t *in, float *bme_raw) {
    static lichen_sensors_t s; // this task only
    lichen_sensors_get(&s);
    int64_t now = esp_timer_get_time();
    *bme_raw    = s.temperature_c;
    in->src[TH_SRC_BOARD] = (th_reading_t){s.env_valid && now - s.env_us < STALE_US,
                                           th_board_from_bme(s.temperature_c, HEATER_ON)};
    in->src[TH_SRC_DIE]   = (th_reading_t){s.die_valid && now - s.die_us < STALE_US, s.die_c};
    uint64_t state = atomic_load(&sim_state);
    if (SIM_MC(state) != NO_SIM) {
        if (up_s() - atomic_load(&sim_since_s) > SIM_MAX_S) {
            sim_write(NO_SIM, false); // this poll still reads the real sensor, the next one sees the new generation
        } else {
            in->src[TH_SRC_BOARD] = (th_reading_t){true, (float)SIM_MC(state) / 1000.0f};
        }
    }
    return SIM_GEN(state);
}

static void publish(th_state_t const *st, th_input_t const *in, float bme_raw) {
    taskENTER_CRITICAL(&lock);
    info.level      = st->level;
    info.blind      = st->blind;
    info.simulated  = SIM_MC(atomic_load(&sim_state)) != NO_SIM;
    info.bme_raw_c  = bme_raw;
    info.cause      = st->cause;
    info.cause_rise = st->cause_rise;
    info.cause_c    = st->cause_c;
    for (int s = 0; s < TH_SRC_COUNT; s++) {
        info.valid[s]       = in->src[s].valid && th_plausible(in->src[s].c);
        info.c[s]           = in->src[s].c;
        info.implausible[s] = st->implausible[s];
    }
    taskEXIT_CRITICAL(&lock);
}

// ---- the first-run notice --------------------------------------------------------------------------------------------

// Firmware before #282 saved the radios off while it shed load: a badge that went down shed under it still has them
// off, and the person is told once. This firmware never sets the key (a hold is not saved).
static void shed_boot_notice(void) {
    if (!settings_get_u8(SHED_KEY, 0)) {
        return;
    }
    if (lichen_notify_to("lichen", LN_WARN, "battery", "Radios were switched off",
                         "Wi-Fi and LoRa were switched off because the badge was too hot. Switch them on again in "
                         "Settings when it has cooled.", 0)) {
        settings_erase(SHED_KEY);
    }
}

static void first_run_notice(void) {
    shed_boot_notice();
    if (settings_get_u8(SEEN_KEY, 0)) {
        return;
    }
    if (lichen_notify_to("lichen", LN_WARN, "battery", "About the batteries",
                         "The cells are unprotected and the badge cannot see them. Use USB or a power bank, never "
                         "charge unattended. Open this for details.", 0)) {
        settings_set_u8(SEEN_KEY, 1);
    }
}

static void thermal_task(void *arg) {
    (void)arg;
    static th_state_t st;
    th_reset(&st);
    unsigned settled = 0;
    uint32_t last_gen = 0; // the simulation generation the board history belongs to
    for (unsigned poll = 0;; poll++) {
        th_input_t in = {0};
        float      bme_raw;
        uint32_t gen = read_sources(&in, &bme_raw);
        if (gen != last_gen) { // the board source changed under this very reading: start a fresh rise window
            th_forget_rise(&st, TH_SRC_BOARD);
            last_gen = gen;
        }
        unsigned ev = th_step(&st, &in, up_s());
        atomic_store(&last_poll_s, up_s());
        atomic_fetch_add(&polls, 1);
        publish(&st, &in, bme_raw);
        if (ev) {
            on_events(ev);
        }
        if (settled < SETTLE_POLLS) {
            settled = !hooks.ui_ready || hooks.ui_ready() ? settled + 1 : 0;
            if (settled == SETTLE_POLLS) {
                first_run_notice();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

// ---- console ---------------------------------------------------------------------------------------------------------

static void print_status(void) {
    lichen_sys_thermal_info_t i;
    lichen_sys_thermal_info(&i);
    if (atomic_load(&running)) {
        console_reply("watchdog: running, %u polls, the last %u s ago (%s)", (unsigned)atomic_load(&polls),
                      (unsigned)(up_s() - atomic_load(&last_poll_s)), hooks.ui_ready ? "with the shell" : "no display");
    } else {
        console_reply("watchdog: NOT RUNNING (no memory for its task): nothing watches the temperature");
    }
    console_reply("thermal: %s%s%s (polled every %d s)", th_level_name(i.level), i.blind ? ", BLIND (no sensor)" : "",
                  i.simulated ? ", SIMULATED board reading" : "", POLL_MS / 1000);
    for (int s = 0; s < TH_SRC_COUNT; s++) {
        th_limits_t l = th_limits((th_src_t)s);
        if (i.valid[s]) {
            console_reply("  %-15s %.1f C  (warn %.0f, clear %.0f, shutdown %.0f, rise %.0f in %d s from %.0f; "
                          "refused %u)", th_src_name((th_src_t)s), (double)i.c[s], (double)l.warn_c,
                          (double)l.clear_c, (double)l.shutdown_c, (double)l.rise_c, TH_WINDOW_S,
                          (double)l.rise_floor_c, (unsigned)i.implausible[s]);
        } else {
            console_reply("  %-15s no reading  (refused %u)", th_src_name((th_src_t)s), (unsigned)i.implausible[s]);
        }
    }
    console_reply("  BME690 raw %.1f C, heater %s: board = raw (no correction, reads warm); C6 temperature is not available",
                  (double)i.bme_raw_c, HEATER_ON ? "on" : "off");
    if (i.level != TH_OK) {
        console_reply("  cause: %s %s at %.1f C", th_src_name(i.cause), i.cause_rise ? "rise" : "level",
                      (double)i.cause_c);
    }
    taskENTER_CRITICAL(&lock);
    unsigned n = event_count < EVENT_LOG ? event_count : EVENT_LOG;
    event_t  copy[EVENT_LOG];
    for (unsigned k = 0; k < n; k++) {
        copy[k] = events[(event_count - 1 - k) % EVENT_LOG];
    }
    taskEXIT_CRITICAL(&lock);
    console_reply(n ? "  last events (newest first):" : "  no events this boot");
    for (unsigned k = 0; k < n; k++) {
        console_reply("    up %us: %s", (unsigned)copy[k].up_s, copy[k].text);
    }
}

static void sim_set(float c) {
    atomic_store(&sim_since_s, up_s());
    sim_write((int32_t)(c * 1000.0f), true);
}

static void cmd_test(char const *what) {
    if (strcmp(what, "warn") == 0) {
        sim_set(SIM_WARN_C);
        console_reply("thermal test: board reads %.0f C; the warning follows in ~%d s (ends by itself after %d s)",
                      (double)SIM_WARN_C, POLL_MS * TH_CONFIRM / 1000, SIM_MAX_S);
    } else if (strcmp(what, "shutdown YES") == 0) {
        sim_set(SIM_SHUTDOWN_C);
        console_reply("thermal test: board reads %.0f C; the badge SWITCHES OFF in ~%d s (power button starts it)",
                      (double)SIM_SHUTDOWN_C, POLL_MS * TH_CONFIRM / 1000);
    } else if (strcmp(what, "shutdown") == 0) {
        console_reply("thermal test shutdown really powers the badge off: add the word YES (test shutdown YES)");
    } else if (strcmp(what, "clear") == 0) {
        sim_write(NO_SIM, false);
        console_reply("thermal test: simulation off; a warning ends after %d cool readings", TH_CONFIRM);
    } else {
        console_reply("thermal test warn | clear | shutdown YES");
    }
}

static void cmd_thermal(char const *args) {
    if (args[0] == '\0') {
        print_status();
    } else if (strncmp(args, "test ", 5) == 0) {
        cmd_test(args + 5);
    } else if (strcmp(args, "seen reset") == 0) {
        settings_erase(SEEN_KEY);
        console_reply("battery notice will show again after the next reboot");
    } else {
        console_reply("thermal | thermal test warn|clear|shutdown YES | thermal seen reset");
    }
}

void thermal_start(lichen_watch_hooks_t const *h) {
    if (h) {
        hooks = *h;
    }
    console_register("thermal", "thermal [test warn|clear|shutdown YES] (temperature watchdog, battery safety)",
                     cmd_thermal);
    // LP RAM (#269): a poll every few seconds that writes NVS (its shed and seen flags), so internal-capable, no DMA
    if (!lichen_task_start_in(NULL, "thermal", thermal_task, NULL, STACK_BYTES, LICHEN_PRIO_SERVICE, LICHEN_STACK_LP_RAM,
                              NULL)) {
        ESP_LOGE(TAG, "no memory: nothing watches the temperature");
        return;
    }
    atomic_store(&running, true);
}
