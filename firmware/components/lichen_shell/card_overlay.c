// "fw ui card [test|white|grey|black|off]" (#352): the bench test card (test_card.c) on top of whatever screen is up. It
// sits on the sys layer, so the screen, its focus and the nav stack under it are not touched and "fw ui" reads the same
// before and after. It goes away on "fw ui card off", on Esc (physical, or `fw keys send esc`) or CARD_AUTO_OFF_MS after
// the last "fw ui card" command, so a script that died cannot leave the panel covered. Nothing is saved.
#include "esp_app_desc.h"
#include "esp_log.h"
#include "fw_console.h"
#include "idle_policy.h"
#include "key_matrix.h"
#include "lichen_reg.h"
#include "lichen_sys_display.h"
#include "lichen_sys_ui.h"
#include "raw_keys.h"
#include "shell_parts.h"
#include "test_card.h"

static char const TAG[] = "card";

#define CARD_AUTO_OFF_MS 60000

static lv_obj_t   *card;
static lv_timer_t *auto_off;

static void on_raw(key_raw_event_t const *ev, void *arg);

static unsigned open_cards; // LVGL task: every test card up, this overlay's and the Test card app's (#355)

// Opening or closing a card is activity: one opened on a dark screen lights it, and the idle clock counts from the close
static void on_card_opened(int delta) {
    open_cards = idle_card_count(open_cards, delta);
    screen_idle_touch();
}

bool test_cards_open(void) {
    return open_cards > 0;
}

static bool listening;   // the raw key listener is registered
static bool eat_release; // Esc closed the card: its release is swallowed too, or it would reach the screen below

static void unlisten(void) {
    if (listening) {
        raw_keys_unlisten(on_raw, NULL);
        listening = false;
    }
}

// LVGL context. keep_keys: stay registered until Esc is released (the physical Esc press closed it)
static void close_card(bool keep_keys) {
    if (!card) {
        return;
    }
    eat_release = keep_keys && listening;
    if (!eat_release) {
        unlisten();
    }
    if (auto_off) {
        lv_timer_delete(auto_off);
        auto_off = NULL;
    }
    lv_obj_delete(card);
    card = NULL;
    ESP_LOGI(TAG, "closed");
}

static void on_auto_off(lv_timer_t *t) {
    (void)t;
    auto_off = NULL; // a one-shot timer deletes itself
    ESP_LOGI(TAG, "auto off");
    close_card(false);
}

// While the card is up the physical keys do nothing else (swallowed); Esc closes it
static void on_raw(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    if (ev->code != KEY_MATRIX_ESC_CODE) {
        return;
    }
    if (ev->pressed) {
        close_card(true);
    } else if (eat_release) {
        eat_release = false;
        unlisten();
    }
}

bool card_overlay_key(uint32_t key) {
    if (!card) {
        return false;
    }
    if (key == LV_KEY_ESC) {
        close_card(false);
    }
    return true; // an open card takes every key, as the physical ones
}

// LVGL task: arg is the tcard_mode_t
static void apply(void *arg) {
    tcard_mode_t mode = (tcard_mode_t)(intptr_t)arg;
    if (mode == TCARD_OFF) {
        close_card(false);
        return;
    }
    if (card) {
        test_card_set_mode(card, mode);
    } else {
        card = test_card_create(lv_layer_sys(), mode);
        eat_release = false;
        listening   = listening || raw_keys_listen(on_raw, NULL, true);
        if (!card || !listening) {
            ESP_LOGE(TAG, "no card (memory or key listener)");
            if (card) {
                lv_obj_delete(card);
                card = NULL;
            }
            return;
        }
    }
    if (!auto_off) {
        auto_off = lv_timer_create(on_auto_off, CARD_AUTO_OFF_MS, NULL);
        lv_timer_set_repeat_count(auto_off, 1);
    } else {
        lv_timer_reset(auto_off);
    }
}

void card_overlay_cmd(char const *args) {
    tcard_mode_t mode;
    if (!tcard_mode_parse(args, &mode)) {
        console_reply("ui card: usage: ui card [test|white|grey|black|off]");
        return;
    }
    if (!lichen_sys_ui_call(apply, (void *)(intptr_t)mode, 0)) {
        console_reply("ui card: no UI");
        return;
    }
    if (mode == TCARD_OFF) {
        console_reply("ui card: off");
    } else {
        console_reply("ui card: on, Esc or `fw ui card off` closes it, %d s", CARD_AUTO_OFF_MS / 1000);
    }
}

void card_overlay_init(void) {
    test_card_set_hooks(&(test_card_hooks_t){
        .fw_label    = esp_app_get_description()->version,
        .reg_get     = lichen_reg_get,
        .display_get = lichen_sys_display_get,
        .opened      = on_card_opened,
    });
}
