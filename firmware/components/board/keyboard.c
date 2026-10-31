#include "keyboard.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_tca8418.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kbd_map.h"

#include <assert.h>
#include <string.h>

static char const TAG[] = "keyboard";

#define SDA_PIN     18
#define SCL_PIN     20
#define POLL_MS     20
#define QUEUE_DEPTH 32

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

static void poll_task(void *arg) {
    (void)arg;
    for (;;) {
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
    chip = tca8418_create(SCL_PIN, SDA_PIN, 0, GPIO_NUM_NC, 0, 0);
    if (!chip) {
        ESP_LOGE(TAG, "TCA8418 not found");
        return false;
    }
    tca8418_flush(chip);
    events = xQueueCreate(QUEUE_DEPTH, sizeof(key_event_t));
    if (!events || xTaskCreatePinnedToCore(poll_task, "keyboard", 3072, NULL, 4, NULL, 0) != pdPASS) {
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
