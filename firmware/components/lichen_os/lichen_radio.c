#include "lichen_os.h"
#include "lichen_radio.h"
#include "lichen_sys_radio.h"

#include "fw_console.h"
#include "settings_store.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

#define KEY_ON    "radio_on"
#define KEY_SAVED "radio_saved"
#define KEY_AIR   "radio_air"

static portMUX_TYPE      lock = portMUX_INITIALIZER_UNLOCKED;
static radio_state_t     state;
static bool              loaded;
static StaticSemaphore_t change_buf;
static SemaphoreHandle_t change_lock; // one change at a time, so listeners hear them in order
static lichen_radio_cb_t listeners[LICHEN_RADIO_LISTENERS];

char const *lichen_radio_name(radio_id_t id) {
    static char const *const names[RADIO_COUNT] = {"Wi-Fi", "LoRa", "Bluetooth"};
    return (unsigned)id < RADIO_COUNT ? names[id] : "?";
}

// NVS is read once, at the first call (boot, before the radios start); the state is only changed through this file
static bool save(radio_state_t const *s);
static void ensure_loaded(void) {
    portENTER_CRITICAL(&lock);
    bool done = loaded;
    portEXIT_CRITICAL(&lock);
    if (done) {
        return;
    }
    radio_state_t s;
    radio_state_load(&s, settings_get_u8(KEY_ON, RADIO_MASK_DEFAULT), settings_get_u8(KEY_SAVED, RADIO_MASK_DEFAULT),
                     settings_get_u8(KEY_AIR, 0));
    portENTER_CRITICAL(&lock);
    if (!loaded) {
        state       = s;
        change_lock = xSemaphoreCreateMutexStatic(&change_buf);
        loaded      = true;
    }
    portEXIT_CRITICAL(&lock);
}

bool lichen_radio_get(radio_id_t id) {
    ensure_loaded();
    portENTER_CRITICAL(&lock);
    bool on = radio_state_get(&state, id);
    portEXIT_CRITICAL(&lock);
    return on;
}

bool lichen_radio_switch_get(radio_id_t id) {
    ensure_loaded();
    portENTER_CRITICAL(&lock);
    bool on = radio_state_switch(&state, id);
    portEXIT_CRITICAL(&lock);
    return on;
}

bool lichen_airplane_get(void) {
    ensure_loaded();
    portENTER_CRITICAL(&lock);
    bool on = state.airplane;
    portEXIT_CRITICAL(&lock);
    return on;
}

bool lichen_radio_listen(lichen_radio_cb_t cb) {
    bool ok = false;
    portENTER_CRITICAL(&lock);
    for (int i = 0; i < LICHEN_RADIO_LISTENERS && !ok; i++) {
        if (!listeners[i]) {
            listeners[i] = cb;
            ok           = true;
        }
    }
    portEXIT_CRITICAL(&lock);
    return ok;
}

static void tell(radio_id_t id, bool on) {
    lichen_radio_cb_t copy[LICHEN_RADIO_LISTENERS];
    portENTER_CRITICAL(&lock);
    memcpy(copy, listeners, sizeof(copy));
    portEXIT_CRITICAL(&lock);
    for (int i = 0; i < LICHEN_RADIO_LISTENERS; i++) {
        if (copy[i]) {
            copy[i](id, on);
        }
    }
}

void lichen_radio_apply_all(void) {
    for (int id = 0; id < RADIO_COUNT; id++) {
        tell((radio_id_t)id, lichen_radio_get((radio_id_t)id));
    }
}

// NVS itself does not write a value that is equal to the stored one
static bool save(radio_state_t const *s) {
    bool ok = settings_set_u8(KEY_ON, s->on);
    ok      = settings_set_u8(KEY_SAVED, s->saved) && ok;
    ok      = settings_set_u8(KEY_AIR, s->airplane ? 1 : 0) && ok;
    return ok;
}

static void tell_diff(uint8_t diff, radio_state_t const *after) {
    uint8_t eff = radio_state_effective(after);
    for (int i = 0; i < RADIO_COUNT; i++) {
        if (diff >> i & 1u) {
            tell((radio_id_t)i, eff >> i & 1u);
        }
    }
}

// The person's switches (on, saved, airplane): what is written to NVS. A hold is not among them (#282).
static bool switches_differ(radio_state_t const *a, radio_state_t const *b) {
    return a->on != b->on || a->saved != b->saved || a->airplane != b->airplane;
}

// What change() does: one radio's switch (overriding a hold, or the switch only, #348) or airplane mode
typedef enum { CHANGE_SET, CHANGE_SWITCH, CHANGE_AIRPLANE } change_op_t;

static uint8_t apply(change_op_t op, radio_id_t id, bool on) {
    switch (op) {
    case CHANGE_SET:
        return radio_state_set(&state, id, on);
    case CHANGE_SWITCH:
        return radio_state_set_switch(&state, id, on);
    case CHANGE_AIRPLANE:
        return radio_state_set_airplane(&state, on);
    default:
        return 0; // an unknown op changes nothing
    }
}

// Applies a change under the lock, then saves and tells outside it. Switching a held radio off changes nothing the
// radio does but is still the person's choice, so it is saved.
static bool change(change_op_t op, radio_id_t id, bool on) {
    ensure_loaded();
    xSemaphoreTake(change_lock, portMAX_DELAY);
    portENTER_CRITICAL(&lock);
    radio_state_t before = state;
    uint8_t       diff   = apply(op, id, on);
    radio_state_t after  = state;
    portEXIT_CRITICAL(&lock);
    bool ok = !switches_differ(&before, &after) || save(&after);
    tell_diff(diff, &after);
    xSemaphoreGive(change_lock);
    return ok;
}

// A hold or its end (#282): the radios are told, nothing is saved, so a reboot meanwhile brings the person's switches
static void hold_change(radio_hold_t reason, bool hold, uint8_t mask) {
    ensure_loaded();
    xSemaphoreTake(change_lock, portMAX_DELAY);
    portENTER_CRITICAL(&lock);
    uint8_t       diff  = hold ? radio_state_hold(&state, reason, mask) : radio_state_release(&state, reason);
    radio_state_t after = state;
    portEXIT_CRITICAL(&lock);
    tell_diff(diff, &after);
    xSemaphoreGive(change_lock);
}

void lichen_sys_radio_hold(radio_hold_t reason, uint8_t mask) {
    hold_change(reason, true, mask);
}

void lichen_sys_radio_release(radio_hold_t reason) {
    hold_change(reason, false, 0);
}

radio_hold_t lichen_sys_radio_held_by(radio_id_t id) {
    ensure_loaded();
    portENTER_CRITICAL(&lock);
    radio_hold_t r = radio_state_held_by(&state, id);
    portEXIT_CRITICAL(&lock);
    return r;
}

bool lichen_radio_set(radio_id_t id, bool on) {
    return change(CHANGE_SET, id, on);
}

bool lichen_radio_switch_set(radio_id_t id, bool on) {
    return change(CHANGE_SWITCH, id, on);
}

bool lichen_airplane_set(bool on) {
    return change(CHANGE_AIRPLANE, RADIO_WIFI, on);
}

// "on", "off", or while the system holds it (#282) "held off (hot)" with the person's switch on and "off, held off
// (hot)" with it off (#348: Settings may switch it off during a hold, and it then stays off once the badge cools)
static char const *state_word(radio_id_t id) {
    if (lichen_radio_get(id)) {
        return "on";
    }
    if (lichen_sys_radio_held_by(id) != RADIO_HOLD_THERMAL) {
        return "off";
    }
    return lichen_radio_switch_get(id) ? "held off (hot)" : "off, held off (hot)";
}

static void cmd_airplane(char const *args) {
    if (strcmp(args, "on") == 0 || strcmp(args, "off") == 0) {
        lichen_airplane_set(args[1] == 'n');
    } else if (*args) {
        console_reply("usage: airplane [on|off]");
        return;
    }
    // Next to the saved switch, what the C6's controller is in (bt.state, #311): the registry's cache, no wait
    char bt[48];
    if (!lichen_reg_cached_text("bt.state", bt, sizeof(bt))) {
        snprintf(bt, sizeof(bt), "?");
    }
    console_reply("airplane %s | wifi %s | lora %s | bluetooth %s, controller %s", lichen_airplane_get() ? "on" : "off",
                  state_word(RADIO_WIFI), state_word(RADIO_LORA), state_word(RADIO_BT), bt);
}

void lichen_radio_console_register(void) {
    console_register("airplane", "airplane [on|off] (all radios off; each can be switched on again; off restores them)",
                     cmd_airplane);
}

// Set once at boot (main's start_shell) before any app runs, read later from the borrowing app's task
static bool (*lora_user_pause)(bool *was_running, bool (*keep_going)(void));
static void (*lora_user_resume)(void);

void lichen_radio_set_lora_user(bool (*pause)(bool *was_running, bool (*keep_going)(void)), void (*resume)(void)) {
    lora_user_pause  = pause;
    lora_user_resume = resume;
}

bool lichen_radio_lora_pause(bool *was_running, bool (*keep_going)(void)) {
    *was_running = false;
    return !lora_user_pause || lora_user_pause(was_running, keep_going);
}

void lichen_radio_lora_resume(void) {
    if (lora_user_resume) {
        lora_user_resume();
    }
}
