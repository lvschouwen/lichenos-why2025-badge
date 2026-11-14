#include "qr_screen.h"

#include "fonts.h"
#include "lichen_ui.h"
#include "qr_payload.h"
#include "screen_ids.h"
#include "theme.h"

#include "esp_attr.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "qr";

#define QR_PX      360 // the code's canvas; the border adds the quiet zone phones need. 4 px per module up to 90 modules
#define QR_BORDER  14
#define SCROLL_STEP 96
#define KIND_CAP   48
#define NAME_CAP   96
#define CAPTION_CAP 336

// What the open screen shows; set by qr_screen_open, read by the builder, wiped when the screen goes (the payload may
// hold a password). LVGL context only.
EXT_RAM_BSS_ATTR static struct {
    char kind[KIND_CAP];
    char name[NAME_CAP];
    char payload[QR_PAYLOAD_MAX + 1];
    char caption[CAPTION_CAP];
} s_qr;

static void on_delete(lv_event_t *e) {
    (void)e;
    memset(&s_qr, 0, sizeof(s_qr));
}

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_target(e), key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
    }
}

static lv_obj_t *add_code(lv_obj_t *screen) {
    lv_obj_t *qr = lv_qrcode_create(screen);
    lv_qrcode_set_size(qr, QR_PX);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_set_style_border_width(qr, QR_BORDER, 0);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_opa(qr, LV_OPA_COVER, 0);
    if (lv_qrcode_update(qr, s_qr.payload, (uint32_t)strlen(s_qr.payload)) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "a payload of %u bytes did not encode", (unsigned)strlen(s_qr.payload));
        lv_obj_delete(qr);
        return NULL;
    }
    return qr;
}

static lv_obj_t *build_qr(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 16, 0);
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);
    shell_scroll_tame(screen);
    lichen_heading(screen, s_qr.kind, s_qr.name);
    bool      coded = add_code(screen) != NULL;
    lv_obj_t *label = lichen_label_wrap(screen, LV_PCT(100));
    lv_obj_set_style_text_font(label, lichen_font(LICHEN_FONT_SMALL), 0);
    if (coded) {
        lv_obj_set_style_text_color(label, lv_color_hex(THEME_TEXT), 0);
        lv_label_set_text(label, s_qr.caption[0] ? s_qr.caption : s_qr.payload);
    } else {
        lv_obj_set_style_text_color(label, lv_color_hex(THEME_RED), 0);
        lv_label_set_text(label, "This does not fit in a QR code: it is too long.");
    }
    shell_page_hint(screen, "Up/Down", "scroll", NULL);
    lichen_ui_event_add(screen, on_key, LV_EVENT_KEY, NULL);
    lichen_ui_event_add(screen, on_delete, LV_EVENT_DELETE, NULL);
    return screen;
}

bool qr_screen_open(char const *kind, char const *name, char const *payload, char const *caption) {
    if (!qr_payload_ok(payload) || shell_screen_open(SCREEN_QR, 0)) {
        return false;
    }
    snprintf(s_qr.kind, sizeof(s_qr.kind), "%s", kind ? kind : "");
    snprintf(s_qr.name, sizeof(s_qr.name), "%s", name ? name : "");
    snprintf(s_qr.payload, sizeof(s_qr.payload), "%s", payload);
    snprintf(s_qr.caption, sizeof(s_qr.caption), "%s", caption ? caption : "");
    shell_open(build_qr, SCREEN_QR);
    return true;
}
