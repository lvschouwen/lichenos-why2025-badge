#include "lichen_radio.h"

#include "fw_console.h"
#include "settings_store.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
static void ensure_loaded(void) {
    portENTER_CRITICAL(&lock);
    bool done = loaded;
    portEXIT_CRITICAL(&lock);
    if (done) {
        return;
    }
    radio_state_t s;
    radio_state_load(&s, settings_get_u8(KEY_ON, RADIO_MASK_ALL), settings_get_u8(KEY_SAVED, RADIO_MASK_ALL),
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

// Applies a change under the lock, then saves and tells outside it
static bool change(radio_id_t id, bool on, int airplane) {
    ensure_loaded();
    xSemaphoreTake(change_lock, portMAX_DELAY);
    portENTER_CRITICAL(&lock);
    radio_state_t before = state;
    uint8_t       diff   = airplane >= 0 ? radio_state_set_airplane(&state, airplane == 1) : radio_state_set(&state, id, on);
    radio_state_t after  = state;
    portEXIT_CRITICAL(&lock);
    if (!diff && before.airplane == after.airplane) {
        xSemaphoreGive(change_lock);
        return true;
    }
    bool ok = save(&after);
    for (int i = 0; i < RADIO_COUNT; i++) {
        if (diff >> i & 1u) {
            tell((radio_id_t)i, after.on >> i & 1u);
        }
    }
    xSemaphoreGive(change_lock);
    return ok;
}

bool lichen_radio_set(radio_id_t id, bool on) {
    return change(id, on, -1);
}

bool lichen_airplane_set(bool on) {
    return change(RADIO_WIFI, false, on ? 1 : 0);
}

static void cmd_airplane(char const *args) {
    if (strcmp(args, "on") == 0 || strcmp(args, "off") == 0) {
        lichen_airplane_set(args[1] == 'n');
    } else if (*args) {
        console_reply("usage: airplane [on|off]");
        return;
    }
    console_reply("airplane %s | wifi %s | lora %s | bluetooth %s", lichen_airplane_get() ? "on" : "off",
                  lichen_radio_get(RADIO_WIFI) ? "on" : "off", lichen_radio_get(RADIO_LORA) ? "on" : "off",
                  lichen_radio_get(RADIO_BT) ? "on" : "off");
}

void lichen_radio_console_register(void) {
    console_register("airplane", "airplane [on|off] (all radios off; each can be switched on again; off restores them)",
                     cmd_airplane);
}
