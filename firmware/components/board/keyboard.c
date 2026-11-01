#include "keyboard.h"

#include "board_i2c.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_tca8418.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kbd_map.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

static char const TAG[] = "keyboard";

#define POLL_MS     20
#define QUEUE_DEPTH 32
#define CHECK_MS    1000 // the self-heal readback (#14): one I2C read a second

static_assert(KBD_KEY_UP == LV_KEY_UP && KBD_KEY_DOWN == LV_KEY_DOWN && KBD_KEY_LEFT == LV_KEY_LEFT &&
                  KBD_KEY_RIGHT == LV_KEY_RIGHT && KBD_KEY_ENTER == LV_KEY_ENTER &&
                  KBD_KEY_BACKSPACE == LV_KEY_BACKSPACE && KBD_KEY_ESC == LV_KEY_ESC && KBD_KEY_NEXT == LV_KEY_NEXT,
              "kbd_map.h's key values must match LVGL's");

typedef struct {
    uint32_t key;
    bool     pressed;
} key_event_t;

static tca8418_dev_t *chip;
static QueueHandle_t  events;
static uint32_t       held[0x51]; // what each pressed key was mapped to, so its release reports the same key
static bool           shift;
static bool           raw_down[0x51]; // keys the raw hooks saw pressed, so a chip reset releases them there too (#14)
static atomic_bool    reinit_asked;            // keyboard_reinit(): honoured by the poll task, which owns the chip
static void (*_Atomic reset_hook)(void);       // told when the readback finds the chip reset (#14)
static bool (*_Atomic wake_hook)(void);        // the screen timeout (#43)
static bool           woke[0x51];              // presses the wake hook swallowed: their releases go nowhere either

static struct {
    key_raw_fn fn;
    void      *arg;
} hooks[KEYBOARD_RAW_MAX];
static portMUX_TYPE hooks_lock = portMUX_INITIALIZER_UNLOCKED;

bool keyboard_raw_add(key_raw_fn fn, void *arg) {
    bool ok = false;
    portENTER_CRITICAL(&hooks_lock);
    for (int i = 0; i < KEYBOARD_RAW_MAX && !ok; i++) {
        if (!hooks[i].fn) {
            hooks[i].fn  = fn;
            hooks[i].arg = arg;
            ok           = true;
        }
    }
    portEXIT_CRITICAL(&hooks_lock);
    return ok;
}

void keyboard_raw_remove(key_raw_fn fn, void *arg) {
    portENTER_CRITICAL(&hooks_lock);
    for (int i = 0; i < KEYBOARD_RAW_MAX; i++) {
        if (hooks[i].fn == fn && hooks[i].arg == arg) {
            hooks[i].fn = NULL;
        }
    }
    portEXIT_CRITICAL(&hooks_lock);
}

// Offers the event to every hook (a copy of the table, so a hook may remove itself); true = swallowed
static bool run_hooks(key_raw_event_t const *ev) {
    typeof(hooks) copy;
    portENTER_CRITICAL(&hooks_lock);
    memcpy(copy, hooks, sizeof(copy));
    portEXIT_CRITICAL(&hooks_lock);
    bool swallowed = false;
    for (int i = 0; i < KEYBOARD_RAW_MAX; i++) {
        if (copy[i].fn && copy[i].fn(ev, copy[i].arg)) {
            swallowed = true;
        }
    }
    return swallowed;
}

void keyboard_reinit(void) {
    atomic_store(&reinit_asked, true);
}

void keyboard_set_reset_hook(void (*fn)(void)) {
    atomic_store(&reset_hook, fn);
}

void keyboard_set_wake_hook(bool (*fn)(void)) {
    atomic_store(&wake_hook, fn);
}

// A press that only woke the screen, or the release of one: nobody else hears of it
static bool wake_swallows(uint8_t key, bool pressed) {
    if (!pressed) {
        bool was  = woke[key];
        woke[key] = false;
        return was;
    }
    bool (*hook)(void) = atomic_load(&wake_hook);
    woke[key]          = hook && hook();
    return woke[key];
}

// The chip forgot its pressed keys with its configuration: release them all, so LVGL never sees a key held forever
static void release_held(void) {
    shift = false;
    memset(woke, 0, sizeof(woke));
    xQueueReset(events); // queued presses are moot now, and the releases must not be dropped by a full queue
    for (size_t k = 0; k < sizeof(held) / sizeof(held[0]); k++) {
        if (raw_down[k]) {
            key_raw_event_t raw_ev = {.code = (uint8_t)k, .pressed = false, .shift = false};
            run_hooks(&raw_ev); // hold-to-exit and other raw listeners would otherwise see the key down forever
            raw_down[k] = false;
        }
        if (held[k]) {
            key_event_t ev = {.key = held[k], .pressed = false};
            xQueueSend(events, &ev, 0);
            held[k] = 0;
        }
    }
}

static void reconfigure(char const *why) {
    tca8418_configure(chip);
    tca8418_flush(chip);
    release_held();
    ESP_LOGW(TAG, "TCA8418 reconfigured (%s)", why);
}

// C6 EN also resets the TCA8418 (#14). Once a second the KP_GPIO1 readback tells whether it still has its setup; a
// failed read (chip held in reset) is no verdict, the next second asks again.
static void self_heal(TickType_t *last_check) {
    if (atomic_exchange(&reinit_asked, false)) {
        reconfigure("asked");
        *last_check = xTaskGetTickCount();
        return;
    }
    if (xTaskGetTickCount() - *last_check < pdMS_TO_TICKS(CHECK_MS)) {
        return;
    }
    *last_check = xTaskGetTickCount();
    if (tca8418_check(chip) == TCA8418_CHECK_RESET) {
        reconfigure("chip was reset");
        void (*hook)(void) = atomic_load(&reset_hook);
        if (hook) {
            hook();
        }
    }
}

static void poll_task(void *arg) {
    (void)arg;
    TickType_t last_check = xTaskGetTickCount();
    for (;;) {
        self_heal(&last_check);
        while (tca8418_get_event_count(chip) > 0) {
            uint8_t raw     = tca8418_get_key(chip);
            uint8_t key     = raw & 0x7F;
            bool    pressed = raw & 0x80;
            if (key == 0 || key >= sizeof(held) / sizeof(held[0])) {
                continue;
            }
            if (kbd_map_is_shift(key)) {
                shift = pressed;
            }
            if (wake_swallows(key, pressed)) {
                continue;
            }
            raw_down[key]          = pressed;
            key_raw_event_t raw_ev = {.code = key, .pressed = pressed, .shift = shift};
            bool swallowed = run_hooks(&raw_ev);
            // A key pressed before the hook grabbed the keys still gets its release, or LVGL would see it held forever
            if (swallowed && (pressed || !held[key])) {
                continue;
            }
            if (kbd_map_is_shift(key)) {
                continue;
            }
            uint32_t mapped = pressed ? kbd_map_key(key, shift) : held[key];
            held[key]       = pressed ? mapped : 0;
            if (mapped) {
                key_event_t ev = {.key = mapped, .pressed = pressed};
                xQueueSend(events, &ev, 0); // a full queue drops keys rather than blocking the poll
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    static uint32_t last_key;
    key_event_t     ev;
    if (xQueueReceive(events, &ev, 0) == pdTRUE) {
        last_key               = ev.key;
        data->key              = ev.key;
        data->state            = ev.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = uxQueueMessagesWaiting(events) > 0;
    } else {
        data->key   = last_key;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bool keyboard_start(lv_indev_t **out) {
    if (!board_i2c_bus()) { // the OS owns the bus; the driver adopts it (#87)
        return false;
    }
    chip = tca8418_create(BOARD_I2C_SCL, BOARD_I2C_SDA, 0, GPIO_NUM_NC, 0, 0);
    if (!chip) {
        ESP_LOGE(TAG, "TCA8418 not found");
        return false;
    }
    tca8418_flush(chip);
    events = xQueueCreate(QUEUE_DEPTH, sizeof(key_event_t));
    if (!events || xTaskCreatePinnedToCore(poll_task, "keyboard", 4096, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "keyboard task not started");
        return false;
    }
    lvgl_port_lock(0);
    *out = lv_indev_create();
    lv_indev_set_type(*out, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(*out, read_cb);
    lvgl_port_unlock();
    return true;
}
