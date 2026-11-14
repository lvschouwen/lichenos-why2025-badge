// The applied radio on MeshCore's radio pages (#335): what the C6 last applied (the registry cache) next to what
// MeshCore asks for, and a note when the two differ. LVGL task only; the reads never wait (mesh_get_settings is a copy,
// lichen_reg_get the cache).
#include "meshcore_screens.h"

#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_radio_line.h"
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define APPLIED_REFRESH_MS 1000 // the registry polls the C6 about this often

// The menu's focused row below the notes, NULL when it is above them (a change in the notes cannot move it) or none is
static lv_obj_t *focused_below(lv_obj_t *menu, lv_obj_t *box) {
    uint32_t n = lv_obj_get_child_count(menu);
    for (uint32_t i = lv_obj_get_index(box) + 1; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(menu, (int32_t)i);
        if (lv_obj_has_state(child, LV_STATE_FOCUSED)) {
            return child;
        }
    }
    return NULL;
}

// The notes' new texts; the note that appears or a line more or less would push every row under it (R7), so the menu
// scrolls by as much and the focused row stays where it was (as far as the menu can scroll)
static void applied_fill(lv_obj_t *box) {
    lv_obj_t      *now     = lv_obj_get_child(box, 0);
    lv_obj_t      *differs = lv_obj_get_child(box, 1);
    lv_obj_t      *menu    = lv_obj_get_parent(box);
    lv_obj_t      *focused = focused_below(menu, box);
    int32_t        was_y   = focused ? lv_obj_get_y(focused) : 0;
    mesh_applied_t a;
    mesh_settings_t s;
    char           line[96], text[160];
    mesh_applied_read(&a);
    mesh_applied_line(&a, false, line, sizeof(line));
    snprintf(text, sizeof(text), "LoRa radio now: %s", line);
    lichen_label_set(now, text);
    bool differ = mesh_get_settings(&s) && mesh_applied_differs(&a, s.freq, s.bw, s.sf, s.cr, s.tx_dbm);
    if (differ) {
        mesh_format_radio(line, sizeof(line), s.freq, s.bw, s.sf, s.cr);
        snprintf(text, sizeof(text), "Differs: MeshCore asks for %s, %d dBm", line, s.tx_dbm);
        lichen_label_set(differs, text);
    }
    lv_obj_set_hidden(differs, !differ);
    if (focused) {
        lv_obj_update_layout(menu);
        int32_t moved = lv_obj_get_y(focused) - was_y;
        if (moved) {
            lv_obj_scroll_to_y(menu, lv_obj_get_scroll_y(menu) + moved, LV_ANIM_OFF);
        }
    }
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
    lv_timer_t *timer = lichen_ui_timer_create(applied_tick, APPLIED_REFRESH_MS, box);
    if (timer) {
        lichen_ui_event_add(box, applied_delete, LV_EVENT_DELETE, timer);
    }
}
