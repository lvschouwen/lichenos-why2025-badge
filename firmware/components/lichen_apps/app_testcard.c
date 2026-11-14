// Test card (#352): the bench test card (lichen_api's test_card.c) as an app with its own tile. Opens on the test card;
// Left and Right cycle test, white, grey and black; Esc leaves. The card sits on the sys layer so it covers the status
// bar too, and goes with the app's screen. No timeout here (that is the console overlay's, `fw ui card`).
#include "lichen_apps.h"
#include "lichen_app.h"
#include "lichen_ui.h"
#include "test_card.h"
#include "theme.h"

#include <stdio.h>

static lv_obj_t    *card;
static tcard_mode_t mode;

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    int      d   = key == LV_KEY_RIGHT ? 1 : key == LV_KEY_LEFT ? -1 : 0;
    if (d && card) {
        mode = tcard_mode_step(mode, d);
        test_card_set_mode(card, mode);
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (card) {
        lv_obj_delete(card);
        card = NULL;
    }
}

static void open_testcard(lv_obj_t *screen) {
    mode = TCARD_TEST;
    card = test_card_create(lv_layer_sys(), mode);
    if (!card) {
        lv_obj_t *l = lv_label_create(screen);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_RED), 0);
        lv_label_set_text(l, "Not enough memory.");
        return;
    }
    lichen_ui_event_add(screen, on_key, LV_EVENT_KEY, NULL);
    lichen_ui_event_add(screen, on_delete, LV_EVENT_DELETE, NULL);
}

static bool testcard_tile(char *l1, size_t c1, char *l2, size_t c2) {
    snprintf(l1, c1, "panel and backlight");
    snprintf(l2, c2, "Left/Right: field");
    return true;
}

// A colour bar strip over a grey wedge on the tile
static void icon_testcard(lv_obj_t *parent, int size_px) {
    static uint32_t const bars[] = {0xE5484D, 0xF5B700, 0x3FC1B0, 0x4D8DF5, 0xC15FE5};
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    int w = size_px * 5 / 8, h = size_px / 5, x0 = (size_px - w) / 2, bw = w / 5;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, x0 + i * bw, size_px / 4);
        lv_obj_set_size(b, bw, h);
        lv_obj_set_style_bg_color(b, lv_color_hex(bars[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_t *g = lv_obj_create(parent);
        lv_obj_remove_style_all(g);
        lv_obj_set_pos(g, x0 + i * bw, size_px / 4 + h + size_px / 10);
        lv_obj_set_size(g, bw, h);
        lv_obj_set_style_bg_color(g, lv_color_hex(0x303030 + (uint32_t)i * 0x303030), 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    }
}

static lichen_app_t const testcard_app = {
    .name             = "Test card",
    .id               = "testcard",
    .draw_icon        = icon_testcard,
    .tile_text        = testcard_tile,
    .open             = open_testcard,
    .caps             = 0,
    .internal_ram_kib = 1,
    .psram_kib        = 0,
};

void testcard_app_register(void) {
    lichen_register_app(&testcard_app);
}
