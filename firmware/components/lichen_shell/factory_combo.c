// The factory reset key combo (#66), for a badge whose UI will not come up: Esc and Backspace (the two ends of the top
// row) held together for 3 s, starting within 10 s of the keyboard coming up (factory_plan.c decides). The TCA8418
// only reports changes, and keyboard_start drops what it queued, so keys held since power-on may not show: that is why
// there is a short window rather than one read; releasing and pressing them again inside it works.
//
// It offers the settings reset only (everything else stays in Settings and the console), on a page of its own on
// LVGL's top layer: no shell screen, menu or dialog. While the question is up the combo hook swallows every key, so
// the UI underneath sees none. If the LVGL lock cannot be had (the UI is stuck) the question still works blind:
// Enter resets, Esc or 30 s cancel, and the log says so.
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "factory_reset.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "keyboard.h"
#include "shell.h"
#include "theme.h"

#include <stdint.h>

static char const TAG[] = "factory";

#define TICK_MS    100
#define LOCK_MS    2000
#define TASK_STACK 6144 // builds the question page (the Power goodbye page needs as much)

static portMUX_TYPE     lock = portMUX_INITIALIZER_UNLOCKED;
static factory_combo_t  combo;
static int64_t          start_us;
static TaskHandle_t     task;
static lv_obj_t        *page; // on the top layer while the question is up

static uint32_t now_ms(void) {
    return (uint32_t)((esp_timer_get_time() - start_us) / 1000);
}

// Keyboard task: quick, no LVGL
static bool on_raw(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    portENTER_CRITICAL(&lock);
    bool       was = factory_combo_swallows(&combo);
    fc_event_t e   = factory_combo_key(&combo, ev->code, ev->pressed, now_ms());
    bool       now = factory_combo_swallows(&combo);
    portEXIT_CRITICAL(&lock);
    if (e != FC_EV_NONE && task) {
        xTaskNotify(task, (uint32_t)e, eSetValueWithOverwrite);
    }
    return was || now; // the Enter or Esc that answered the question too
}

static void add_label(lv_obj_t *parent, char const *text, uint32_t colour) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, text);
}

static void show_question(void) {
    if (!lvgl_port_lock(LOCK_MS)) {
        ESP_LOGW(TAG, "boot keys: no screen (UI stuck); Enter resets the settings, Esc cancels");
        return;
    }
    page = lv_obj_create(lv_layer_top());
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, THEME_PAD, 0);
    lv_obj_set_style_pad_top(page, THEME_PAD * 3, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 24, 0);
    add_label(page, "Factory reset: settings", THEME_AMBER);
    add_label(page, "Erases Wi-Fi, time zone, display, radio switches, notification and app settings, then restarts "
                    "into first-run setup.", THEME_TEXT);
    add_label(page, "MeshCore keeps its identity, contacts and channels. Files and the SD card are not touched.",
              THEME_DIM);
    add_label(page, "Enter: reset settings and restart\nEsc: cancel (by itself after 30 s)", THEME_TEXT);
    lvgl_port_unlock();
}

static void close_question(void) {
    if (page && lvgl_port_lock(LOCK_MS)) {
        lv_obj_delete(page);
        page = NULL;
        lvgl_port_unlock();
    }
}

static void handle(fc_event_t e) {
    switch (e) {
    case FC_EV_ARM:
        if (lichen_screen_locked()) {
            // The pocket lock (#75) is unlocked with the same two keys: never also a reset question. It cannot engage
            // this early (30 s idle at the least), so only "fw lock now" gets here.
            portENTER_CRITICAL(&lock);
            combo.state = FC_OVER;
            portEXIT_CRITICAL(&lock);
            ESP_LOGI(TAG, "boot keys held while locked: no reset question");
            break;
        }
        ESP_LOGI(TAG, "boot keys held: asking for a settings reset");
        show_question();
        break;
    case FC_EV_CONFIRM:
        ESP_LOGI(TAG, "boot keys: settings reset confirmed");
        if (factory_reset_request(FACTORY_SETTINGS)) {
            system_restart("factory reset settings (boot keys)");
        }
        ESP_LOGE(TAG, "boot keys: reset not saved, nothing erased");
        close_question();
        break;
    case FC_EV_CANCEL:
        ESP_LOGI(TAG, "boot keys: reset cancelled");
        close_question();
        break;
    default:
        break;
    }
}

static void combo_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t v = FC_EV_NONE;
        xTaskNotifyWait(0, UINT32_MAX, &v, pdMS_TO_TICKS(TICK_MS));
        handle((fc_event_t)v);
        portENTER_CRITICAL(&lock);
        fc_event_t e    = factory_combo_tick(&combo, now_ms());
        bool       over = combo.state == FC_OVER;
        portEXIT_CRITICAL(&lock);
        handle(e);
        if (over) {
            break;
        }
    }
    keyboard_raw_remove(on_raw, NULL);
    // A notification the hook sent between the last wait and the remove: Enter or Esc on an open question
    uint32_t v = FC_EV_NONE;
    if (xTaskNotifyWait(0, UINT32_MAX, &v, 0) == pdTRUE) {
        handle((fc_event_t)v);
    }
    close_question();
    task = NULL;
    vTaskDelete(NULL);
}

void factory_combo_start(void) {
    factory_combo_init(&combo);
    start_us = esp_timer_get_time();
    if (xTaskCreatePinnedToCore(combo_task, "factory_keys", TASK_STACK, NULL, 3, &task, 1) != pdPASS) {
        ESP_LOGE(TAG, "no boot key combo: no task");
        task = NULL;
        return;
    }
    if (!keyboard_raw_add(on_raw, NULL)) {
        ESP_LOGE(TAG, "no boot key combo: no raw key hook free");
    }
}
