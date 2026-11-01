// Notifications on screen (#62): the toast on lv_layer_top() under the status bar, the tile badges, the cloud key.
// lichen_notify() queues toasts from any task; the drain timer here takes them on the LVGL task (the pattern of
// raw_keys.c) through notify_view.c's queue, and reads only copies of the entries. The toast is never clickable, in no
// group and never focused: Enter and Esc always stay with the screen under it (Lucas, #62 Q1). The cloud key acts on
// the showing toast (open it), otherwise opens the centre.
#include "esp_log.h"
#include "fonts.h"
#include "lichen_app.h"
#include "lichen_badge.h"
#include "lichen_clock.h"
#include "notify_os.h"
#include "notify_view.h"
#include "shell_parts.h"
#include "theme.h"
#include "update_screen.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "notify_ui";

#define DRAIN_MS  100
#define TOAST_W   640
#define TOAST_GAP 12
#define STRIPE_W  10

static lv_obj_t     *s_toast; // the box; hidden while nothing shows
static lv_obj_t     *s_stripe;
static lv_obj_t     *s_title;
static lv_obj_t     *s_body;
static nview_queue_t s_waiting;
static nview_toast_t s_showing;   // id 0: none
static uint32_t      s_shown_at;  // lv_tick of the last (re)show
static uint32_t      s_gen = UINT32_MAX; // lichen_notify_generation() at the last check; none yet: the first drain sets the badges
static bool          s_badged[LICHEN_MAX_APPS]; // a badge this file set (so it also clears it, and no other)

uint32_t notify_level_color(uint8_t level) {
    return level >= LN_ERROR ? THEME_LINK_RED : level == LN_WARN ? THEME_AMBER : THEME_DIM;
}

static void hide(void) {
    if (!s_showing.id) {
        return;
    }
    if (s_showing.feedback) {
        lichen_notify_feedback((ln_level_t)s_showing.level, false); // the LED goes off with the toast
    }
    s_showing.id = 0;
    lv_obj_set_hidden(s_toast, true);
}

// Fills the box from the entry; false when it is gone or already read (nothing to show)
static bool fill(uint32_t id) {
    static nr_entry_t e; // LVGL task only
    if (!lichen_notify_get(id, &e) || e.read) {
        return false;
    }
    char title[NR_TITLE_CAP + 16];
    nview_title(title, sizeof(title), e.title, e.count);
    lv_label_set_text(s_title, title);
    lv_label_set_text(s_body, e.body);
    lv_obj_set_style_bg_color(s_stripe, lv_color_hex(notify_level_color(e.level)), 0);
    return true;
}

static void show_next(void) {
    nview_toast_t next;
    while (nview_pop(&s_waiting, &next)) {
        if (fill(next.id)) {
            s_showing  = next;
            s_shown_at = lv_tick_get();
            lv_obj_set_hidden(s_toast, false);
            if (next.feedback) {
                lichen_notify_feedback((ln_level_t)next.level, true);
            }
            return;
        }
    }
}

// Tile badges (#102): each source app's unread count; only badges this file set are ever cleared here, so the crash
// log's and MeshCore's own counts are left alone
static void update_badges(void) {
    for (size_t i = 0; i < lichen_app_count() && i < LICHEN_MAX_APPS; i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (!app->id || strcmp(app->id, NOTIFY_APP_ID) == 0) {
            continue;
        }
        size_t n = lichen_notify_unread(app->id);
        if (n || s_badged[i]) {
            lichen_badge_set(app->id, (unsigned)n);
            s_badged[i] = n > 0;
        }
    }
    lichen_badge_set(NOTIFY_APP_ID, (unsigned)lichen_notify_unread(NULL));
}

static void drain(lv_timer_t *timer) {
    (void)timer;
    ln_toast_t t;
    while (lichen_notify_next_toast(&t)) {
        nview_toast_t const v = {.id = t.id, .level = t.level, .feedback = t.feedback};
        if (s_showing.id == t.id) {
            fill(t.id); // a merged notice: new text, the time starts again
            s_shown_at = lv_tick_get();
        } else if (!nview_push(&s_waiting, v)) {
            ESP_LOGI(TAG, "toast %u dropped: queue full", (unsigned)t.id);
        }
    }
    uint32_t gen = lichen_notify_generation();
    if (gen != s_gen) {
        s_gen = gen;
        update_badges();
        if (s_showing.id && !fill(s_showing.id)) {
            hide(); // read or cleared meanwhile (the centre, the app's own "seen")
        }
    }
    if (s_showing.id && lv_tick_elaps(s_shown_at) >= nview_toast_ms(s_showing.level)) {
        hide();
    }
    // Not over the boot screen nor the update screen; the queue waits
    if (!s_showing.id && s_waiting.n && shell_home_shown() && !update_screen_busy()) {
        show_next();
    }
}

// The cloud key (#79's shortcuts.c decided it may act now)
void notify_ui_cloud_key(void) {
    if (s_showing.id) {
        uint32_t id = s_showing.id;
        hide();
        notify_open_entry(id);
    } else {
        notify_open_centre();
    }
}

static lv_obj_t *add_label(lv_obj_t *parent, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_max_height(label, 2 * lv_font_get_line_height(lichen_font(LICHEN_FONT_SMALL)), 0);
    return label;
}

// Made before the status bar and any dialog: as the first child of the top layer it stays under both
void notify_ui_start(void) {
    lichen_notify_set_clock(clock_is_set);
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, TOAST_W, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, THEME_STATUS_BAR_H + TOAST_GAP);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 14, 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(THEME_RULE), 0);
    lv_obj_set_style_pad_all(s_toast, 16, 0);
    lv_obj_set_style_pad_left(s_toast, 16 + STRIPE_W + 14, 0);
    lv_obj_set_style_text_font(s_toast, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_toast, 4, 0);
    lv_obj_set_scrollable(s_toast, false);
    lv_obj_set_clickable(s_toast, false); // never takes a key or the focus (Lucas, #62 Q1)
    s_stripe = lv_obj_create(s_toast);
    lv_obj_remove_style_all(s_stripe);
    lv_obj_set_ignore_layout(s_stripe, true);
    lv_obj_set_size(s_stripe, STRIPE_W, LV_PCT(100));
    lv_obj_align(s_stripe, LV_ALIGN_LEFT_MID, -(STRIPE_W + 14), 0);
    lv_obj_set_style_radius(s_stripe, STRIPE_W / 2, 0);
    lv_obj_set_style_bg_opa(s_stripe, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_stripe, false);
    s_title = add_label(s_toast, THEME_TEXT);
    s_body  = add_label(s_toast, THEME_DIM);
    lv_obj_set_hidden(s_toast, true);
    lv_timer_create(drain, DRAIN_MS, NULL);
}
