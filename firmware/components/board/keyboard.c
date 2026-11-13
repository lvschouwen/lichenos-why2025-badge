#include "keyboard.h"

#include "board_i2c.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_tca8418.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kbd_map.h"
#include "key_origin.h"
#include "raw_hooks.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

static char const TAG[] = "keyboard";

#define POLL_MS     20
#define QUEUE_DEPTH 32
#define INJECT_DEPTH 16 // keyboard_inject's queue: a held uppercase character is two events, a tap four at most
#define CHECK_MS    1000 // the self-heal readback (#14): one I2C read a second

static_assert(KBD_KEY_UP == LV_KEY_UP && KBD_KEY_DOWN == LV_KEY_DOWN && KBD_KEY_LEFT == LV_KEY_LEFT &&
                  KBD_KEY_RIGHT == LV_KEY_RIGHT && KBD_KEY_ENTER == LV_KEY_ENTER &&
                  KBD_KEY_BACKSPACE == LV_KEY_BACKSPACE && KBD_KEY_ESC == LV_KEY_ESC && KBD_KEY_NEXT == LV_KEY_NEXT,
              "kbd_map.h's key values must match LVGL's");

typedef struct {
    uint32_t key;
    bool     pressed;
    bool     injected; // key_origin_injected: read_cb records it for lichen_sys_keys_last_injected (#391)
} key_event_t;

typedef struct {
    uint8_t      code;
    bool         pressed;
    key_origin_t origin; // key_origin_of_inject(as_physical)
} injected_t;

static tca8418_dev_t *chip;
static QueueHandle_t  events;
static _Atomic(QueueHandle_t) injected; // keyboard_inject (#375): drained by the poll task ahead of the chip
static uint32_t       held[0x51]; // what each pressed key was mapped to, so its release reports the same key
static bool           shift;
static bool           raw_down[0x51]; // keys the raw hooks saw pressed, so a chip reset releases them there too (#14)
static key_origin_t   held_origin[0x51]; // key_origin_stamp: a release reports its press's origin (#391)
static atomic_bool    reinit_asked;            // keyboard_reinit(): honoured by the poll task, which owns the chip
static void (*_Atomic reset_hook)(void);       // told when the readback finds the chip reset (#14)
static bool (*_Atomic wake_hook)(void);        // the screen timeout (#43)
static bool (*_Atomic gate_hook)(uint8_t key, bool pressed); // the remote key lock (#190)
static void (*_Atomic gate_reset_hook)(void);                // ... told when every key was released
static bool           woke[0x51];              // presses the wake hook swallowed: their releases go nowhere either
static atomic_bool    last_injected = true;    // read_cb's last event was injected (#391); true: no hand yet (#400)

static raw_hooks_t hooks;              // raw_hooks.h: no lock, so this task never waits on a hook's remover (#251)
static _Atomic(TaskHandle_t) poll_handle; // the keyboard task: a hook that adds or removes a hook runs here
// One add or remove at a time from the other tasks, held through a remover's wait. The keyboard task never takes it:
// its holder may be waiting for the hook call the keyboard task is in.
static StaticSemaphore_t            mutators_buf;
static _Atomic(SemaphoreHandle_t)   mutators;
static atomic_int                   mutators_once; // 0 none, 1 being made, 2 made

static_assert(KEYBOARD_RAW_MAX == RAW_HOOKS_MAX, "keyboard.h promises the table's size");

// Made on first use: a hook may be added before keyboard_start
static SemaphoreHandle_t mutators_lock(void) {
    int none = 0;
    if (atomic_compare_exchange_strong(&mutators_once, &none, 1)) {
        atomic_store(&mutators, xSemaphoreCreateMutexStatic(&mutators_buf)); // static: cannot fail
        atomic_store(&mutators_once, 2);
    }
    while (atomic_load(&mutators_once) != 2) {
        vTaskDelay(1);
    }
    SemaphoreHandle_t m = atomic_load(&mutators);
    xSemaphoreTake(m, portMAX_DELAY);
    return m;
}

static bool in_hook(void) {
    return xTaskGetCurrentTaskHandle() == atomic_load(&poll_handle);
}

bool keyboard_raw_add(key_raw_fn fn, void *arg) {
    if (in_hook()) {
        return raw_hooks_add(&hooks, fn, arg); // lock-free: the table's own exchange guards the claim
    }
    SemaphoreHandle_t m  = mutators_lock();
    bool              ok = raw_hooks_add(&hooks, fn, arg);
    xSemaphoreGive(m);
    return ok;
}

static void wait_a_tick(void) {
    vTaskDelay(1);
}

void keyboard_raw_remove(key_raw_fn fn, void *arg) {
    if (in_hook()) {
        raw_hooks_remove(&hooks, fn, arg, NULL); // no lock and no wait: the call in flight is this task's own
        return;
    }
    SemaphoreHandle_t m = mutators_lock();
    raw_hooks_remove(&hooks, fn, arg, wait_a_tick);
    xSemaphoreGive(m);
}

// Offers the event to every hook; true = swallowed
static bool run_hooks(key_raw_event_t const *ev) {
    return raw_hooks_run(&hooks, ev);
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

void keyboard_set_gate_hook(bool (*fn)(uint8_t key, bool pressed), void (*reset)(void)) {
    atomic_store(&gate_hook, fn);
    atomic_store(&gate_reset_hook, reset);
}

static bool gate_drops(uint8_t key, bool pressed) {
    bool (*gate)(uint8_t, bool) = atomic_load(&gate_hook);
    return gate && gate(key, pressed);
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
    void (*gate_reset)(void) = atomic_load(&gate_reset_hook);
    if (gate_reset) {
        gate_reset(); // the gate's dropped presses get no release from the chip now either
    }
    shift = false;
    memset(woke, 0, sizeof(woke));
    xQueueReset(events); // queued presses are moot now, and the releases must not be dropped by a full queue
    for (size_t k = 0; k < sizeof(held) / sizeof(held[0]); k++) {
        // The held key's own origin: an injected key stays injected through the chip's reset
        bool from_inject = key_origin_injected(key_origin_stamp(&held_origin[k], false, KEY_ORIGIN_CHIP));
        if (raw_down[k]) {
            key_raw_event_t raw_ev = {.code = (uint8_t)k, .pressed = false, .shift = false, .injected = from_inject};
            run_hooks(&raw_ev); // hold-to-exit and other raw listeners would otherwise see the key down forever
            raw_down[k] = false;
        }
        if (held[k]) {
            key_event_t ev = {.key = held[k], .pressed = false, .injected = from_inject};
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

// One key event, the chip's or an injected one (#375): the gate (unless the origin skips it), the wake hook, the raw
// hooks, then LVGL; the origin travels with it to the last two (#391). Poll task only.
static void handle_key(uint8_t key, bool pressed, key_origin_t origin) {
    if (key == 0 || key >= sizeof(held) / sizeof(held[0])) {
        return;
    }
    if (kbd_map_is_shift(key)) {
        shift = pressed;
    }
    if ((key_origin_gated(origin) && gate_drops(key, pressed)) || wake_swallows(key, pressed)) {
        return;
    }
    // Per key: a release is as injected as its press, whichever path it came by
    bool from_inject       = key_origin_injected(key_origin_stamp(&held_origin[key], pressed, origin));
    raw_down[key]          = pressed;
    key_raw_event_t raw_ev = {.code = key, .pressed = pressed, .shift = shift, .injected = from_inject};
    bool swallowed = run_hooks(&raw_ev);
    // A key pressed before the hook grabbed the keys still gets its release, or LVGL would see it held forever
    if (swallowed && (pressed || !held[key])) {
        return;
    }
    if (kbd_map_is_shift(key)) {
        return;
    }
    uint32_t mapped = pressed ? kbd_map_key(key, shift) : held[key];
    held[key]       = pressed ? mapped : 0;
    if (mapped) {
        key_event_t ev = {.key = mapped, .pressed = pressed, .injected = from_inject};
        xQueueSend(events, &ev, 0); // a full queue drops keys rather than blocking the poll
    }
}

bool keyboard_inject(uint8_t code, bool pressed, bool as_physical) {
    QueueHandle_t q = atomic_load(&injected);
    if (!q || code == 0 || code >= sizeof(held) / sizeof(held[0])) {
        return false;
    }
    injected_t in = {.code = code, .pressed = pressed, .origin = key_origin_of_inject(as_physical)};
    return xQueueSend(q, &in, 0) == pdTRUE;
}

static void poll_task(void *arg) {
    (void)arg;
    atomic_store(&poll_handle, xTaskGetCurrentTaskHandle());
    TickType_t last_check = xTaskGetTickCount();
    for (;;) {
        self_heal(&last_check);
        injected_t in;
        while (xQueueReceive(atomic_load(&injected), &in, 0) == pdTRUE) {
            handle_key(in.code, in.pressed, in.origin);
        }
        while (tca8418_get_event_count(chip) > 0) {
            uint8_t raw = tca8418_get_key(chip);
            handle_key(raw & 0x7F, raw & 0x80, KEY_ORIGIN_CHIP);
        }
        // The poll's pause, cut short by an injected event: a held key's press and release then reach the hooks as
        // far apart as they were sent, which is what a hold timed to the millisecond (the remote lock's 3 s) needs
        xQueuePeek(atomic_load(&injected), &in, pdMS_TO_TICKS(POLL_MS));
    }
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    static uint32_t last_key;
    key_event_t     ev;
    if (xQueueReceive(events, &ev, 0) == pdTRUE) {
        last_key = ev.key;
        // Before LVGL handles it: lv_indev_read processes each read before the next, so a key handler asking
        // lichen_sys_keys_last_injected hears about the event it is handling
        atomic_store(&last_injected, ev.injected);
        data->key              = ev.key;
        data->state            = ev.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = uxQueueMessagesWaiting(events) > 0;
    } else {
        data->key   = last_key;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bool keyboard_last_injected(void) {
    return atomic_load(&last_injected);
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
    atomic_store(&injected, xQueueCreate(INJECT_DEPTH, sizeof(injected_t)));
    if (!events || !atomic_load(&injected) || xTaskCreatePinnedToCore(poll_task, "keyboard", 3072 /* peak 1 KB, #107 */, NULL, 4, NULL, 0) != pdPASS) {
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
