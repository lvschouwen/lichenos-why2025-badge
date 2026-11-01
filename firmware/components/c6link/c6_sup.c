// The C6 supervisor (#14): noticing a C6 reset at runtime and bringing the link back without a reboot.
//
// Signals (ESP-Hosted's TRANSPORT_FAILURE, the keyboard finding its chip reset, LoRa sends failing, the console) only
// queue. The c6sup task is the only place recovery runs: it feeds them to the pure watch (c6_watch.c, host-tested)
// and carries out its actions: down hooks, the c6wifi task's driver teardown, esp_hosted deinit + init + connect, the
// driver again, up hooks (keyboard, backlight, LoRa), a note in the restart history. When the C6 does not come back,
// it asks (main.c's dialog) whether to restart the badge once. No LVGL here: the screen polls c6link_status().
#include "c6link.h"
#include "c6link_priv.h"

#include "c6_watch.h"
#include "driver/gpio.h"
#include "eh_host_event.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "nvs.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "c6sup";

ESP_EVENT_DEFINE_BASE(C6LINK_EVENT);

#define RESET_DELAY_MS 1000 // fw c6 reset: lets the console reply leave over Wi-Fi first
#define RESET_PULSE_US 10000
#define ROM_ATTEMPTS   C6W_ATTEMPTS // fw c6 reset rom: every attempt of one outage meets a C6 in download mode
#define PROBE_WAIT_MS  2000         // the LoRa task answers within its 1 s queue wait plus one round trip
#define WIFI_WAIT_MS   20000        // MSG_C6_DOWN: two RPCs of at most 5 s each to a C6 that may be gone
#define QUEUE_DEPTH    8
// esp_hosted_init/connect (SDMMC card init, its tasks), the restart note (NVS blob) and the reason texts. The high-water
// marks of c6sup and c6wifi are logged after every recovery, and fw mem shows them (#14 T-B).
#define SUP_STACK      6144
#define ASK_TRIES      5 // the dialog could not be shown (LVGL busy): ask again once a second, then restart
#define KEY_RT_RESTART "c6_rt_restart" // the badge restarted for a lost C6 (cleared after 10 min up)

typedef enum { SUP_SIGNAL, SUP_REATTACH, SUP_ANSWER } sup_type_t;

typedef struct {
    uint8_t  type;
    bool     restart; // SUP_ANSWER
} sup_msg_t;

static c6_watch_t         watch;
static portMUX_TYPE       watch_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t      sup_queue;
static c6link_hooks_t     hooks;
static esp_timer_handle_t reset_timer;

// Raised signals, coalesced until c6sup takes them (a flood of TRANSPORT_FAILURE posts must not fill the queue and
// push out a console command or the dialog's answer). The time is when the first one was raised, for c6_watch_stale.
static portMUX_TYPE sig_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         sig_hard[2];  // [0] transport, [1] keyboard
static uint32_t     sig_hard_ms[2];
static unsigned     sig_soft_fails;
static uint32_t     sig_soft_ms;
static bool         sig_soft_ok;
static bool         sig_queued;   // a SUP_SIGNAL is in the queue

// The boot attach worked, so the Wi-Fi stack exists: before that there is nothing to recover (signals are ignored)
static atomic_bool boot_attached;
static atomic_bool soft_failing;  // the last LoRa send failed
static atomic_bool hosted_quiet;  // c6sup tears ESP-Hosted down: its failure posts until TRANSPORT_UP are ours
static atomic_int  rom_attempts;  // fw c6 reset rom: attempts left with GPIO13 low
static atomic_bool pulse_rom;     // the pending fw c6 reset is the rom variant
static bool        rt_flag;       // NVS c6_rt_restart was set at boot (c6sup only after start)
static bool        asking;        // the restart question is open (c6sup only)
static int         ask_left;      // fallback pending: tries left to show the question (c6sup only)
static bool        boot_ok;       // the boot attach worked; recovery is enabled once the hooks are in too
static TaskHandle_t sup_handle;
static uint32_t    outage_uptime; // the C6's uptime read before this outage's first re-attach (ms or UNKNOWN)

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void post(sup_msg_t const *m) {
    if (!sup_queue || xQueueSend(sup_queue, m, 0) != pdTRUE) {
        ESP_LOGE(TAG, "queue full: message %u dropped", (unsigned)m->type);
    }
}

// Any task (the event loop, the keyboard, LoRa senders): mark it and make sure one SUP_SIGNAL is queued
static void raise_signal(int hard_idx, bool soft_fail, bool soft_ok) {
    if (!atomic_load(&boot_attached)) {
        return;
    }
    uint32_t t = now_ms();
    portENTER_CRITICAL(&sig_lock);
    if (hard_idx >= 0 && !sig_hard[hard_idx]) {
        sig_hard[hard_idx]    = true;
        sig_hard_ms[hard_idx] = t;
    }
    if (soft_fail && sig_soft_fails++ == 0) {
        sig_soft_ms = t;
    }
    sig_soft_ok = sig_soft_ok || soft_ok;
    bool need   = !sig_queued;
    sig_queued  = true;
    portEXIT_CRITICAL(&sig_lock);
    if (need) {
        sup_msg_t m = {.type = SUP_SIGNAL};
        if (!sup_queue || xQueueSend(sup_queue, &m, 0) != pdTRUE) {
            portENTER_CRITICAL(&sig_lock);
            sig_queued = false; // the next signal tries again
            portEXIT_CRITICAL(&sig_lock);
        }
    }
}

void c6link_note_keyboard_reset(void) {
    raise_signal(1, false, false);
}

void c6link_note_lora_send(bool ok) {
    if (!ok) {
        atomic_store(&soft_failing, true);
        raise_signal(-1, true, false);
    } else if (atomic_exchange(&soft_failing, false)) {
        raise_signal(-1, false, true); // only the first good send after failures: no message per packet
    }
}

void c6link_fallback_answer(bool restart) {
    sup_msg_t m = {.type = SUP_ANSWER, .restart = restart};
    post(&m);
}

void c6link_set_hooks(c6link_hooks_t const *h) {
    hooks = *h; // once from main.c; c6sup reads it only after boot_attached is set below (no torn copy)
    if (boot_ok) {
        atomic_store(&boot_attached, true); // from now on a C6 reset is recovered from
    }
}

// Runs on the event loop task: only notes it
static void on_hosted_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg, (void)base, (void)data;
    if (id == EH_HOST_EVENT_TRANSPORT_UP) {
        atomic_store(&hosted_quiet, false);
    } else if (id == EH_HOST_EVENT_TRANSPORT_FAILURE && !atomic_load(&hosted_quiet)) {
        raise_signal(0, false, false);
    }
}

// ---- carrying out the watch's actions (c6sup only) ----------------------------------------------------------------

static void log_source(char const *what, unsigned act) {
    char line[C6W_REASON_MAX + 64];
    c6_sup_watch_line(line, sizeof(line));
    ESP_LOGW(TAG, "%s: actions 0x%02x, %s", what, act, line);
}

// One hard signal (idx 0 transport, 1 keyboard) that may belong to an outage that is already over
static unsigned take_hard(int idx, uint32_t raised_ms) {
    c6w_source_t src = idx == 0 ? C6W_SRC_TRANSPORT : C6W_SRC_KEYBOARD;
    portENTER_CRITICAL(&watch_lock);
    bool     stale = c6_watch_stale(&watch, src, raised_ms);
    unsigned act   = stale ? 0 : c6_watch_hard(&watch, src, now_ms());
    portEXIT_CRITICAL(&watch_lock);
    if (stale) {
        ESP_LOGW(TAG, "%s from the last outage: ignored", idx == 0 ? "transport failure" : "keyboard reset");
    } else if (act) {
        log_source(idx == 0 ? "ESP-Hosted transport failure" : "keyboard chip was reset", act);
    }
    return act;
}

static unsigned take_signals(void) {
    portENTER_CRITICAL(&sig_lock);
    bool     hard[2] = {sig_hard[0], sig_hard[1]};
    uint32_t hard_ms[2] = {sig_hard_ms[0], sig_hard_ms[1]};
    unsigned fails = sig_soft_fails;
    uint32_t soft_ms = sig_soft_ms;
    bool     ok = sig_soft_ok;
    sig_hard[0] = sig_hard[1] = false;
    sig_soft_fails = 0;
    sig_soft_ok = sig_queued = false;
    portEXIT_CRITICAL(&sig_lock);
    unsigned act = 0;
    for (int i = 0; i < 2; i++) {
        act |= hard[i] ? take_hard(i, hard_ms[i]) : 0;
    }
    portENTER_CRITICAL(&watch_lock);
    if (ok) {
        c6_watch_soft_ok(&watch);
    }
    bool stale = c6_watch_stale(&watch, C6W_SRC_LORA, soft_ms);
    for (unsigned i = 0; i < fails && i < C6W_SOFT_LIMIT && !stale; i++) {
        act |= c6_watch_soft_fail(&watch, now_ms());
    }
    portEXIT_CRITICAL(&watch_lock);
    return act;
}

// LoRa's STATUS carries the C6's uptime: the only thing that tells a C6 that rebooted from one that only lost the
// transport, and it must be read before our own connect_to_slave pulses EN (CP_RESET_STRATEGY_ALWAYS)
static bool probe(uint32_t *uptime_ms) {
    uint32_t s = 0;
    if (!hooks.probe || !hooks.probe(PROBE_WAIT_MS, &s)) {
        return false;
    }
    *uptime_ms = s > UINT32_MAX / 1000 ? UINT32_MAX - 1 : s * 1000;
    return true;
}

static unsigned do_probe(void) {
    uint32_t up       = 0;
    bool     answered = probe(&up);
    ESP_LOGW(TAG, "LoRa sends failing: probe %s", answered ? "answered" : "not answered");
    portENTER_CRITICAL(&watch_lock);
    unsigned act = c6_watch_probe(&watch, answered, now_ms());
    portEXIT_CRITICAL(&watch_lock);
    return act;
}

// The uptime for the verdict, when it is worth asking: a keyboard reset or our own pulse already prove that EN went
// low, and LoRa's probe has just gone unanswered
static void read_outage_uptime(void) {
    portENTER_CRITICAL(&watch_lock);
    bool ask = watch.source != C6W_SRC_KEYBOARD && watch.source != C6W_SRC_LORA && !watch.manual_cause;
    portEXIT_CRITICAL(&watch_lock);
    outage_uptime = C6W_UPTIME_UNKNOWN;
    if (ask && !probe(&outage_uptime)) {
        outage_uptime = C6W_UPTIME_UNKNOWN;
    }
}

static void go_down(void) {
    read_outage_uptime(); // while LoRa may still reach it
    c6link_set_link(false);
    esp_event_post(C6LINK_EVENT, C6LINK_EVENT_DOWN, NULL, 0, 0);
    if (hooks.down) {
        hooks.down();
    }
    // ESP-Hosted must not be deinitialised under an esp_wifi call in flight: wait once more before going on anyway
    if (!c6link_wifi_request(false, WIFI_WAIT_MS) && !c6link_wifi_request(false, WIFI_WAIT_MS)) {
        ESP_LOGE(TAG, "the Wi-Fi task did not take the driver down in %d s, re-attaching anyway", 2 * WIFI_WAIT_MS / 1000);
    }
}

// fw c6 reset rom keeps the strap low for this outage's attempts, so the C6 boots its ROM and every attach fails
static void set_boot_strap(void) {
    int left = atomic_load(&rom_attempts);
    if (left > 0) {
        atomic_store(&rom_attempts, left - 1);
        hold_c6_boot(0);
        ESP_LOGW(TAG, "fw c6 reset rom: this attempt with GPIO13 low (%d more)", left - 1);
    } else {
        hold_c6_boot(1);
    }
}

// esp_hosted 3.0.10 within one boot (#36, never tried before #14): deinit re-arms the bus init. A failed esp_hosted_init
// stays failed until the next boot (INIT_FAILED is sticky); a failed connect is retried after the next deinit.
static unsigned do_reattach(void) {
    uint32_t t0 = now_ms();
    atomic_store(&hosted_quiet, true);
    // One line per step: a step that blocks for good (bench 2026-11-01, second attempt against a C6 in its ROM) is
    // then named by the last line in the console
    ESP_LOGW(TAG, "re-attach: esp_hosted deinit");
    int d = esp_hosted_deinit();
    set_boot_strap();
    ESP_LOGW(TAG, "re-attach: esp_hosted init (deinit %d, %u ms)", d, (unsigned)(now_ms() - t0));
    int i = esp_hosted_init();
    ESP_LOGW(TAG, "re-attach: connect to slave (init %d, %u ms)", i, (unsigned)(now_ms() - t0));
    int  c  = i == 0 ? esp_hosted_connect_to_slave() : -1;
    bool ok = i == 0 && c == 0;
    portENTER_CRITICAL(&watch_lock);
    unsigned attempt = watch.attempt;
    unsigned act     = c6_watch_attached(&watch, ok, outage_uptime, now_ms());
    portEXIT_CRITICAL(&watch_lock);
    ESP_LOGW(TAG, "re-attach attempt %u: %s (deinit %d, init %d, connect %d, %u ms)", attempt, ok ? "ok" : "failed", d,
             i, c, (unsigned)(now_ms() - t0));
    return act;
}

static void go_up(void) {
    if (!c6link_wifi_request(true, WIFI_WAIT_MS)) {
        ESP_LOGE(TAG, "the Wi-Fi task did not bring the driver up in %d s", WIFI_WAIT_MS / 1000);
    }
    // TRANSPORT_UP normally cleared it long ago (the driver bring-up took seconds); if a flood of failure posts made the
    // event loop drop it, a later real failure must still be heard
    if (atomic_exchange(&hosted_quiet, false)) {
        ESP_LOGW(TAG, "TRANSPORT_UP not seen: listening to transport failures again");
    }
    c6link_set_link(true);
    if (hooks.up) {
        hooks.up(); // keyboard, backlight, LoRa: main.c keeps that order
    }
    esp_event_post(C6LINK_EVENT, C6LINK_EVENT_UP, NULL, 0, 0);
    if (asking) {
        asking = false;
        if (hooks.ask_cancel) {
            hooks.ask_cancel();
        }
        ESP_LOGW(TAG, "the C6 is back: restart question withdrawn");
    }
}

// A notification for #62 (the notification centre does not exist yet)
static void notify_c6_event(char const *text) {
    (void)text;
}

static void note(void) {
    char why[C6W_REASON_MAX + 16];
    portENTER_CRITICAL(&watch_lock);
    snprintf(why, sizeof(why), "C6 restarted: %s", watch.reason);
    portEXIT_CRITICAL(&watch_lock);
    system_restart_note(why); // NVS history: fw crash shows it after a later reboot too (cut to its line length)
    notify_c6_event(why);
    ESP_LOGW(TAG, "stack free: c6sup %u, c6wifi %u", (unsigned)uxTaskGetStackHighWaterMark(sup_handle),
             c6link_wifi_stack_free());
}

static void restart_for_c6(char const *why) __attribute__((noreturn));
static void restart_for_c6(char const *why) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, KEY_RT_RESTART, 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    c6link_set_verify_flag(); // the next boot checks (and if needed reflashes) the C6 fully
    system_restart(why);
}

static void fallback(void) {
    bool updating = c6link_updating();
    if (c6_watch_fallback_plan(updating, hooks.ask_restart != NULL) == C6W_FALLBACK_RESTART_NOW) {
        restart_for_c6(updating ? "C6 lost during an update" : "C6 lost at runtime");
    }
    ask_left = ASK_TRIES;
}

// Shows the question; an LVGL lock that stays busy is tried again on the next ticks, and only then the restart comes
// without asking (the countdown would have ended in it too)
static void ask_tick(void) {
    if (ask_left <= 0) {
        return;
    }
    portENTER_CRITICAL(&watch_lock);
    bool up = watch.state == C6W_UP;
    portEXIT_CRITICAL(&watch_lock);
    if (up) {
        ask_left = 0; // back before it could even be asked
        return;
    }
    if (hooks.ask_restart()) {
        ask_left = 0;
        asking   = true;
        ESP_LOGW(TAG, "C6 not responding: asking whether to restart (the countdown restarts)");
    } else if (--ask_left == 0) {
        restart_for_c6("C6 lost at runtime (no dialog)");
    }
}

static void answer(bool restart) {
    portENTER_CRITICAL(&watch_lock);
    bool up = watch.state == C6W_UP;
    portEXIT_CRITICAL(&watch_lock);
    if (!asking) {
        return; // withdrawn: the C6 came back in the meantime
    }
    asking = false;
    if (up) {
        ESP_LOGW(TAG, "the C6 is back: no restart");
    } else if (restart) {
        restart_for_c6("C6 lost at runtime");
    } else {
        ESP_LOGW(TAG, "staying without the C6: retrying every %u min", (unsigned)(C6W_RETRY_SLOW_MS / 60000));
    }
}

static void run(unsigned act) {
    while (act) {
        unsigned next = 0;
        if (act & C6W_ACT_PROBE) {
            next |= do_probe();
        }
        if (act & C6W_ACT_DOWN_HOOKS) {
            go_down();
        }
        if (act & C6W_ACT_REATTACH) {
            next |= do_reattach();
        }
        if (act & C6W_ACT_UP_HOOKS) {
            go_up();
        }
        if (act & C6W_ACT_NOTE) {
            note();
        }
        if (act & C6W_ACT_GIVE_UP) {
            log_source("C6 not coming back", act);
        }
        if (act & C6W_ACT_FALLBACK_RESTART) {
            fallback();
        }
        act = next;
    }
}

// c6_rt_restart is cleared once the C6 has been up for 10 minutes: a later loss may restart the badge again
static void rt_flag_tick(void) {
    portENTER_CRITICAL(&watch_lock);
    c6w_state_t state = watch.state;
    uint32_t    up_for = watch.restarts ? now_ms() - watch.up_ms : now_ms(); // since it came back, or since boot
    portEXIT_CRITICAL(&watch_lock);
    if (!c6_watch_rt_flag_clear_due(rt_flag, state, up_for)) {
        return;
    }
    rt_flag = false;
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, KEY_RT_RESTART);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGW(TAG, "C6 up for %u min: %s cleared", (unsigned)(C6W_RT_FLAG_CLEAR_MS / 60000), KEY_RT_RESTART);
}

// A trial image becomes valid a minute or so after boot: from then on a lost C6 may offer the restart again
static void trial_tick(void) {
    static uint32_t next;
    if (!watch.trial_image || (int32_t)(now_ms() - next) < 0) {
        return; // trial_image is written only here, on this task: reading it without the lock is fine
    }
    next = now_ms() + 10000;
    if (!c6link_image_is_trial()) {
        portENTER_CRITICAL(&watch_lock);
        watch.trial_image = false;
        portEXIT_CRITICAL(&watch_lock);
    }
}

static void sup_task(void *arg) {
    (void)arg;
    for (;;) {
        sup_msg_t m;
        unsigned  act = 0;
        if (xQueueReceive(sup_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (m.type == SUP_SIGNAL) {
                act = take_signals();
            } else if (m.type == SUP_REATTACH) {
                portENTER_CRITICAL(&watch_lock);
                act = c6_watch_reattach(&watch, now_ms());
                portEXIT_CRITICAL(&watch_lock);
                log_source("fw c6 reattach", act);
            } else {
                answer(m.restart);
            }
        }
        portENTER_CRITICAL(&watch_lock);
        act |= c6_watch_tick(&watch, now_ms());
        portEXIT_CRITICAL(&watch_lock);
        run(act);
        ask_tick();
        rt_flag_tick();
        trial_tick();
    }
}

// ---- status -------------------------------------------------------------------------------------------------------

void c6_sup_watch_line(char *out, size_t cap) {
    portENTER_CRITICAL(&watch_lock);
    c6_watch_t copy = watch;
    portEXIT_CRITICAL(&watch_lock);
    c6_watch_line(&copy, now_ms(), out, cap);
}

void c6link_status(c6_status_t *out) {
    portENTER_CRITICAL(&watch_lock);
    out->state            = watch.state;
    out->source           = watch.source;
    out->restarts         = watch.restarts;
    out->last_recovery_ms = watch.last_recovery_ms;
    out->attempt          = watch.attempt;
    memcpy(out->reason, watch.reason, sizeof(out->reason));
    portEXIT_CRITICAL(&watch_lock);
}

bool c6link_trouble(char *out, size_t cap) {
    portENTER_CRITICAL(&watch_lock);
    c6w_state_t state   = watch.state;
    uint8_t     attempt = watch.attempt;
    portEXIT_CRITICAL(&watch_lock);
    if (state == C6W_UP) {
        return false;
    }
    c6_watch_state_text(state, attempt, out, cap);
    return true;
}

// ---- console: fw c6 reset [rom], fw c6 reattach -------------------------------------------------------------------

// A raw EN pulse behind ESP-Hosted's back, as a C6 crash would look (#14): detection has to find it on its own. GPIO12
// is already an output (ESP-Hosted's reset pin, set up by the attach). GPIO13 (the C6 GPIO9 strap) is held high so the
// C6 boots its application, or low for the rom variant (download mode for this outage's attempts).
static void reset_pulse(void *arg) {
    (void)arg;
    bool rom = atomic_load(&pulse_rom);
    if (c6link_updating() || !c6link_up()) { // checked again: an update may have started in the second since the command
        ESP_LOGW(TAG, "fw c6 reset: not now (%s)", c6link_updating() ? "update running" : "no C6 link");
        return;
    }
    portENTER_CRITICAL(&watch_lock);
    c6_watch_manual_reset(&watch, now_ms());
    portEXIT_CRITICAL(&watch_lock);
    atomic_store(&rom_attempts, rom ? ROM_ATTEMPTS : 0);
    hold_c6_boot(rom ? 0 : 1);
    ESP_LOGW(TAG, "fw c6 reset%s: C6 EN low for %d ms", rom ? " rom" : "", RESET_PULSE_US / 1000);
    gpio_set_level(GPIO_NUM_12, 0);
    esp_rom_delay_us(RESET_PULSE_US);
    gpio_set_level(GPIO_NUM_12, 1);
}

static void cmd_reset(bool rom) {
    if (c6link_updating() || !reset_timer || !c6link_up()) {
        console_reply("c6 reset: not now (%s)", c6link_updating() ? "update running" : "no C6 link");
        return;
    }
    if (rom) {
        console_reply("C6 reset into its ROM in %d s, only with someone at the badge: the %d re-attach attempts fail, "
                      "then a dialog offers a restart in 30 s (Esc stays; the 5-min retry then boots it normally). "
                      "Read the result with fw c6, fw crash and the log", RESET_DELAY_MS / 1000, ROM_ATTEMPTS);
    } else {
        console_reply("C6 reset in %d s, Wi-Fi drops for about 10 s while c6sup re-attaches: read the result with fw c6, "
                      "fw crash and the log", RESET_DELAY_MS / 1000);
    }
    atomic_store(&pulse_rom, rom);
    esp_timer_stop(reset_timer); // a second fw c6 reset within the second: still one pulse
    esp_timer_start_once(reset_timer, (uint64_t)RESET_DELAY_MS * 1000);
}

bool c6_sup_command(char const *args) {
    if (strcmp(args, "reset") == 0 || strcmp(args, "reset rom") == 0) {
        cmd_reset(args[5] != '\0');
        return true;
    }
    if (strcmp(args, "reattach") != 0) {
        return false;
    }
    if (c6link_updating() || !sup_queue || !atomic_load(&boot_attached)) {
        console_reply("c6 reattach: not now (%s)", !sup_queue                     ? "no supervisor"
                                                   : !atomic_load(&boot_attached) ? "no C6 at boot: restart instead"
                                                                                  : "update running");
        return true;
    }
    sup_msg_t m = {.type = SUP_REATTACH};
    post(&m);
    console_reply("c6 reattach: queued, Wi-Fi drops for about 10 s (the re-attach resets the C6); fw c6 shows the result");
    return true;
}

// ---- start ----------------------------------------------------------------------------------------------------------

static bool read_rt_flag(void) {
    nvs_handle_t nvs;
    uint8_t      v = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, KEY_RT_RESTART, &v);
        nvs_close(nvs);
    }
    return v != 0;
}

void c6_sup_start(void) {
    rt_flag = read_rt_flag();
    c6_watch_init(&watch, c6link_image_is_trial(), rt_flag);
    esp_timer_create_args_t const args = {.callback = reset_pulse, .name = "c6reset"};
    if (esp_timer_create(&args, &reset_timer) != ESP_OK) {
        reset_timer = NULL;
    }
    sup_queue = xQueueCreate(QUEUE_DEPTH, sizeof(sup_msg_t));
    if (!sup_queue || xTaskCreatePinnedToCore(sup_task, "c6sup", SUP_STACK, NULL, 5, &sup_handle, 0) != pdPASS) {
        ESP_LOGE(TAG, "C6 supervisor not started: a C6 reset is not recovered from");
    }
}

void c6_sup_attached(void) {
    boot_ok = true; // recovery itself waits for c6link_set_hooks
    // esp_hosted_init created the default event loop. Registered once: the handlers outlive esp_hosted_deinit.
    esp_err_t err = esp_event_handler_instance_register(EH_HOST_EVENT, EH_HOST_EVENT_TRANSPORT_FAILURE, on_hosted_event,
                                                        NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(EH_HOST_EVENT, EH_HOST_EVENT_TRANSPORT_UP, on_hosted_event, NULL, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no ESP-Hosted event handler: %s", esp_err_to_name(err));
    }
}
