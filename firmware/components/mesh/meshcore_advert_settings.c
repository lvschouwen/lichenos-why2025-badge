// Settings > MeshCore > Advert & contacts (#52): the periodic advert, the manual advert's reach, the location and
// whether adverts carry it, and the auto-add rules. One page of rows: Enter cycles a value or opens a text entry, each
// change runs in a short-lived task on core 0 (the mesh.h setters may wait for the mesh lock) and the page shows the
// result. The rules and texts are mesh_advert_cfg.c's (host-tested); the screens never wait on MeshCore.
#include "lichen_mem.h"
#include "lichen_task.h"
#include "meshcore_run.h"
#include "screen_ids.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "meshcore_settings.h"
#include "text_entry.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "meshcore";

#define TASK_STACK    6144 // savePrefs keeps File objects on the stack
#define REFRESH_MS    1000
#define UI_CALL_TRIES 100 // lichen_ui_call tries (up to 2.1 s each) before a change's result is dropped

typedef enum { CH_INTERVAL, CH_MANUAL, CH_LOCATION, CH_SHARE, CH_AUTOADD } change_kind_t;

typedef struct {
    change_kind_t    kind;
    uint32_t         minutes;
    bool             flag;
    double           lat, lon;
    advcfg_autoadd_t aa;
    char const      *reason; // result: NULL = applied
} change_t;

static bool busy; // LVGL task only: one change at a time

static struct {
    lv_obj_t   *interval, *next, *manual;
    lv_obj_t   *lat, *lon, *share;
    lv_obj_t   *aa_on, *aa_types[4], *aa_hops, *aa_overwrite;
    lv_obj_t   *note;
    lv_obj_t   *loc_note; // a refused coordinate, under the Longitude row where the user is looking
    lv_timer_t *timer;
} ui;

static uint8_t const TYPE_BITS[4]  = {ADVCFG_AA_CHAT, ADVCFG_AA_REPEATER, ADVCFG_AA_ROOM, ADVCFG_AA_SENSOR};
static char const *const TYPE_NAME[4] = {"Chat", "Repeaters", "Rooms", "Sensors"};

static void set_note(char const *text, bool problem) {
    if (ui.note) {
        lv_label_set_text(ui.note, text);
        lv_obj_set_style_text_color(ui.note, lv_color_hex(problem ? THEME_RED : THEME_DIM), 0);
    }
}

// A refused coordinate shows under the Location rows, not in the page's one note at the bottom (off screen from them)
static void set_loc_note(char const *text) {
    if (!ui.loc_note) {
        return;
    }
    if (text) {
        lv_label_set_text(ui.loc_note, text);
        lv_obj_remove_flag(ui.loc_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.loc_note, LV_OBJ_FLAG_HIDDEN);
    }
}

// All rows from the settings copy (never waits)
static void rows_refresh(lv_timer_t *t) {
    (void)t;
    mesh_advert_cfg_t c;
    if (!ui.interval || !mesh_get_advert_cfg(&c)) {
        return;
    }
    char text[48], badge[16];
    advcfg_interval_text(c.interval_min, text, sizeof(text));
    menu_set_value(ui.interval, text);
    if (c.next_in_s == UINT32_MAX) {
        menu_set_value(ui.next, c.interval_min ? "When the clock is set" : "Off");
    } else {
        advcfg_next_text(c.next_in_s, badge, sizeof(badge));
        menu_set_value(ui.next, badge);
    }
    menu_set_value(ui.manual, c.flood_manual ? "Flood" : "Zero-hop");
    bool have_loc = advcfg_location_set(c.lat, c.lon);
    snprintf(text, sizeof(text), "%.6f", c.lat);
    menu_set_value(ui.lat, have_loc ? text : "Not set");
    snprintf(text, sizeof(text), "%.6f", c.lon);
    menu_set_value(ui.lon, have_loc ? text : "Not set");
    menu_set_on(ui.share, c.share_loc);
    menu_set_on(ui.aa_on, c.autoadd.on);
    for (int i = 0; i < 4; i++) {
        menu_set_on(ui.aa_types[i], (c.autoadd.types & TYPE_BITS[i]) != 0);
    }
    advcfg_hops_text(c.autoadd.max_hops, text, sizeof(text));
    menu_set_value(ui.aa_hops, text);
    menu_set_on(ui.aa_overwrite, c.autoadd.overwrite);
}

// ---- the change, off the LVGL task ---------------------------------------------------------------------------------

static void change_finish(void *data) {
    change_t *ch = data;
    busy         = false;
    if (ch->reason) {
        set_note(ch->reason, true);
    } else {
        set_note("Saved", false);
    }
    rows_refresh(NULL);
    lichen_free(meshcore_ctx(), ch);
}

static void change_task(void *arg) {
    change_t *ch = arg;
    switch (ch->kind) {
        case CH_INTERVAL: ch->reason = mesh_set_advert_interval(ch->minutes); break;
        case CH_MANUAL: ch->reason = mesh_set_advert_manual_flood(ch->flag); break;
        case CH_LOCATION: ch->reason = mesh_set_location(ch->lat, ch->lon); break;
        case CH_SHARE: ch->reason = mesh_set_location_share(ch->flag); break;
        case CH_AUTOADD: ch->reason = mesh_set_autoadd(&ch->aa); break;
    }
    // change_finish frees ch and clears busy. Each try waits up to 2 s for the LVGL lock: after UI_CALL_TRIES the LVGL
    // task is stuck, so the result is dropped here and the page is not left busy for good
    for (int tries = 0; !lichen_ui_call(change_finish, ch); tries++) {
        if (tries + 1 >= UI_CALL_TRIES) {
            ESP_LOGW(TAG, "advert change %d: result not handed to the screen, dropped", (int)ch->kind);
            lichen_free(meshcore_ctx(), ch);
            __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void change_start(change_t const *what) {
    if (busy) {
        set_note(mc_busy_note(), true);
        return;
    }
    change_t *ch = lichen_alloc(meshcore_ctx(), sizeof(*ch), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!ch) {
        set_note("Out of memory", true);
        return;
    }
    *ch = *what;
    if (!lichen_task_start(meshcore_ctx(), "meshadv", change_task, ch, TASK_STACK, LICHEN_PRIO_JOB, NULL)) {
        ESP_LOGW(TAG, "advert settings task did not start");
        lichen_free(meshcore_ctx(), ch);
        set_note(mc_busy_note(), true);
        return;
    }
    busy = true;
    set_note("Saving...", false);
}

// ---- the rows --------------------------------------------------------------------------------------------------------

static void cycle_interval(void *arg) {
    (void)arg;
    mesh_advert_cfg_t c;
    if (mesh_get_advert_cfg(&c)) {
        change_start(&(change_t){.kind = CH_INTERVAL, .minutes = advcfg_interval_cycle(c.interval_min)});
    }
}

static void toggle_manual(void *arg) {
    (void)arg;
    mesh_advert_cfg_t c;
    if (mesh_get_advert_cfg(&c)) {
        change_start(&(change_t){.kind = CH_MANUAL, .flag = !c.flood_manual});
    }
}

static void on_coord(char const *text, void *arg) {
    bool              is_lat = arg != NULL;
    mesh_advert_cfg_t c;
    double            v;
    char const       *why = advcfg_parse_coord(text, is_lat, &v);
    if (why) {
        set_loc_note(why);
        return;
    }
    set_loc_note(NULL);
    if (mesh_get_advert_cfg(&c)) {
        change_start(&(change_t){.kind = CH_LOCATION, .lat = is_lat ? v : c.lat, .lon = is_lat ? c.lon : v});
    }
}

static void edit_coord(void *arg) {
    bool              is_lat = arg != NULL;
    mesh_advert_cfg_t c;
    char              now[24] = "";
    if (mesh_get_advert_cfg(&c) && advcfg_location_set(c.lat, c.lon)) {
        snprintf(now, sizeof(now), "%.6f", is_lat ? c.lat : c.lon);
    }
    if (!text_entry_open(is_lat ? "Latitude in degrees (-90 to 90)" : "Longitude in degrees (-180 to 180)", now, false, 16,
                         on_coord, arg)) {
        set_note("A text entry is already open", true);
    }
}

static void toggle_share(void *arg) {
    (void)arg;
    mesh_advert_cfg_t c;
    if (mesh_get_advert_cfg(&c)) {
        change_start(&(change_t){.kind = CH_SHARE, .flag = !c.share_loc});
    }
}

// Which part of the auto-add setting an Enter changes
typedef enum { AA_ON, AA_TYPE, AA_HOPS, AA_OVERWRITE } aa_part_t;

static void change_autoadd(void *arg) {
    intptr_t          code = (intptr_t)arg;
    aa_part_t         part = (aa_part_t)(code & 0xFF);
    int               type = (int)(code >> 8);
    mesh_advert_cfg_t c;
    if (!mesh_get_advert_cfg(&c)) {
        return;
    }
    advcfg_autoadd_t a = c.autoadd;
    switch (part) {
        case AA_ON:
            a.on = !a.on;
            if (a.on && (a.types & ADVCFG_AA_TYPES) == 0) {
                a.types = ADVCFG_AA_TYPES; // turning it on with no types would add nothing
            }
            break;
        case AA_TYPE:
            a.types ^= TYPE_BITS[type];
            a.on = (a.types & ADVCFG_AA_TYPES) != 0; // no type left is off
            break;
        case AA_HOPS: a.max_hops = advcfg_hops_cycle(a.max_hops); break;
        case AA_OVERWRITE: a.overwrite = !a.overwrite; break;
    }
    change_start(&(change_t){.kind = CH_AUTOADD, .aa = a});
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (ui.timer) {
        lv_timer_delete(ui.timer);
    }
    memset(&ui, 0, sizeof(ui));
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Advert & contacts");
    menu_note(menu, "Advertising");
    ui.interval = menu_add(menu, "Advert every", "", cycle_interval, NULL);
    ui.next     = menu_add(menu, "Next advert", "", NULL, NULL);
    ui.manual   = menu_add(menu, "Advert button sends", "", toggle_manual, NULL);
    menu_set_kind(ui.interval, MENU_CYCLE);
    menu_set_kind(ui.next, MENU_INFO);
    menu_set_kind(ui.manual, MENU_CYCLE);
    menu_note(menu, "Location");
    ui.lat   = menu_add(menu, "Latitude", "", edit_coord, (void *)1);
    ui.lon   = menu_add(menu, "Longitude", "", edit_coord, NULL);
    ui.loc_note = menu_note(menu, "");
    lv_obj_set_style_text_color(ui.loc_note, lv_color_hex(THEME_RED), 0);
    lv_obj_add_flag(ui.loc_note, LV_OBJ_FLAG_HIDDEN);
    ui.share = menu_add(menu, "Share location in advert", "", toggle_share, NULL);
    menu_set_kind(ui.share, MENU_TOGGLE);
    menu_note(menu, "Auto-add contacts");
    ui.aa_on = menu_add(menu, "Auto-add", "", change_autoadd, (void *)(intptr_t)AA_ON);
    for (int i = 0; i < 4; i++) {
        ui.aa_types[i] = menu_add(menu, TYPE_NAME[i], "", change_autoadd, (void *)(intptr_t)(AA_TYPE | (i << 8)));
    }
    ui.aa_hops      = menu_add(menu, "Max hops", "", change_autoadd, (void *)(intptr_t)AA_HOPS);
    ui.aa_overwrite = menu_add(menu, "Overwrite oldest when full", "", change_autoadd, (void *)(intptr_t)AA_OVERWRITE);
    menu_set_kind(ui.aa_on, MENU_TOGGLE);
    for (int i = 0; i < 4; i++) {
        menu_set_kind(ui.aa_types[i], MENU_TOGGLE);
    }
    menu_set_kind(ui.aa_hops, MENU_CYCLE);
    menu_set_kind(ui.aa_overwrite, MENU_TOGGLE);
    ui.note = menu_note(menu, "Advert every: Enter steps through Off, 3 h ... 7 d (flood adverts).");
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    ui.timer = lv_timer_create(rows_refresh, REFRESH_MS, NULL); // "next advert" counts down
    rows_refresh(NULL);
    return screen;
}

void meshcore_advert_settings_open(void *arg) {
    (void)arg;
    shell_open(build, SCREEN_ADVERT);
}
