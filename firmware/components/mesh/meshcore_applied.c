// The applied radio on MeshCore's radio pages (#335): what the C6 last applied (the registry cache) next to what
// MeshCore asks for, and a note when the two differ. LVGL task only; the reads never wait (mesh_get_settings is a copy,
// lichen_reg_get the cache).
#include "meshcore_screens.h"

#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_radio_line.h"
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define APPLIED_REFRESH_MS 1000 // the registry polls the C6 about this often

// Only when the text changed: the labels wrap, and an unchanged rewrite would still relayout the page
static void set_if_changed(lv_obj_t *label, char const *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void applied_fill(lv_obj_t *box) {
    lv_obj_t      *now     = lv_obj_get_child(box, 0);
    lv_obj_t      *differs = lv_obj_get_child(box, 1);
    mesh_applied_t a;
    mesh_settings_t s;
    char           line[96], text[160];
    mesh_applied_read(&a);
    mesh_applied_line(&a, false, line, sizeof(line));
    snprintf(text, sizeof(text), "LoRa radio now: %s", line);
    set_if_changed(now, text);
    bool differ = mesh_get_settings(&s) && mesh_applied_differs(&a, s.freq, s.bw, s.sf, s.cr, s.tx_dbm);
    if (differ) {
        mesh_format_radio(line, sizeof(line), s.freq, s.bw, s.sf, s.cr);
        snprintf(text, sizeof(text), "Differs: MeshCore asks for %s, %d dBm", line, s.tx_dbm);
        set_if_changed(differs, text);
    }
    lv_obj_set_hidden(differs, !differ);
}

static void applied_tick(lv_timer_t *timer) {
    applied_fill(lv_timer_get_user_data(timer));
}

static void applied_delete(lv_event_t *e) {
    lv_timer_t *timer = lv_event_get_user_data(e);
    lv_timer_delete(timer);
}

void meshcore_applied_notes(lv_obj_t *menu) {
    lv_obj_t *box = lv_obj_create(menu);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    menu_note(box, "");                                   // what the radio runs now
    lv_obj_t *differs = menu_note(box, "");               // amber and in words (R8): MeshCore's settings, not applied
    lv_obj_set_style_text_color(differs, lv_color_hex(THEME_AMBER), 0);
    applied_fill(box);
    lv_timer_t *timer = lv_timer_create(applied_tick, APPLIED_REFRESH_MS, box);
    if (timer) {
        lv_obj_add_event_cb(box, applied_delete, LV_EVENT_DELETE, timer);
    }
}
