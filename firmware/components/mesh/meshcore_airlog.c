// The MeshCore app's Air log page (#165): the packets MeshCore received, newest first, from the RAM ring in
// mesh_task.cpp (mesh_airlog_snapshot: a short locked copy, never the mesh lock). Headers only: type, route, hops, signal,
// the last hop's hash and the age. No message text or payload bytes are kept or shown. Receive only.
#include "lichen_mem.h"
#include "meshcore_run.h"
#include "esp_log.h"
#include "lichen_ui.h"
#include "mesh.h"
#include "mesh_format.h"
#include "meshcore_list.h"
#include "meshcore_screens.h"
#include "screen_ids.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "meshcore";

#define SHOW_ENTRIES 100 // rows on screen; the ring keeps AIRLOG_CAP
#define AGE_REFRESH_S 10

typedef struct {
    airlog_entry_t entries[SHOW_ENTRIES];
    uint32_t       keys[SHOW_ENTRIES];
    scroll_item_t  items[SHOW_ENTRIES];
} airlog_mem_t;

static airlog_mem_t *mem; // PSRAM, while the screen exists
static mc_list_t      list;
static lv_timer_t    *timer;
static lv_obj_t      *screen_obj;
static uint32_t       shown_seq, last_rebuild_s;
static uint32_t       sel_seq; // the highlighted entry, kept when new packets push the rows down
static bool           sel_valid;

static void settle_selection(size_t count) {
    size_t at = list.sel < count ? list.sel : (count ? count - 1 : 0);
    for (size_t i = 0; sel_valid && i < count; i++) {
        if (mem->entries[i].seq == sel_seq) {
            at = i;
            break;
        }
    }
    list.sel  = at;
    sel_valid = count > 0;
    if (sel_valid) {
        sel_seq = mem->entries[at].seq;
    }
}

static void add_entry_row(airlog_entry_t const *e, uint32_t now_s, bool selected) {
    char title[64], age[24], detail[96];
    airlog_title(e, title, sizeof(title));
    mesh_format_age(now_s > e->at_s ? now_s - e->at_s : 0, age, sizeof(age));
    airlog_detail(e, age, detail, sizeof(detail));
    lv_obj_t *row = mc_list_row(&list, selected);
    lv_obj_set_width(mc_list_line(row, title, THEME_TEXT), LV_PCT(100)); // full width: DOT cuts a long line
    lv_obj_set_width(mc_list_line(row, detail, THEME_DIM), LV_PCT(100));
}

static void rebuild(size_t count) {
    mc_scroll_t s = mc_list_save(&list);
    lv_obj_clean(list.list);
    list.n = count;
    settle_selection(count);
    if (!count) {
        mc_list_label(list.list,
                      mesh_running() ? "No packets yet: they appear as the radio hears them. Adverts come every few minutes."
                                     : "MeshCore is not running.",
                      THEME_DIM);
        return;
    }
    uint32_t now_s = mesh_uptime_s();
    for (size_t i = 0; i < count; i++) {
        list.keys[i] = scroll_key_hash(&mem->entries[i].seq, sizeof(uint32_t), 0);
        add_entry_row(&mem->entries[i], now_s, i == list.sel);
    }
    mc_list_restore(&list, s);
}

#define REBUILD_MIN_MS 3000
static uint32_t last_rebuild_ms;

static void refresh(lv_timer_t *t) {
    if (t && lv_screen_active() != screen_obj) {
        return;
    }
    uint32_t seq;
    size_t   count = mesh_airlog_snapshot(mem->entries, SHOW_ENTRIES, &seq);
    bool     stale = mesh_uptime_s() - last_rebuild_s >= AGE_REFRESH_S;
    // A busy mesh brings a packet about every second: rebuilding 100 rows each time costs frames, so new packets
    // show at most every REBUILD_MIN_MS (review #165). The first fill (t == NULL) is immediate.
    bool due = !t || lv_tick_elaps(last_rebuild_ms) >= REBUILD_MIN_MS;
    if ((seq != shown_seq && due) || stale) {
        rebuild(count);
        shown_seq       = seq;
        last_rebuild_s  = mesh_uptime_s();
        last_rebuild_ms = lv_tick_get();
    }
}

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if ((key == LV_KEY_UP || key == LV_KEY_DOWN) && mc_list_move(&list, key == LV_KEY_UP ? -1 : 1)) {
        sel_seq = mem->entries[list.sel].seq;
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    lichen_free(meshcore_self(), mem);
    mem        = NULL;
    screen_obj = NULL;
    list       = (mc_list_t){0};
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    screen_obj       = screen;
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    mem = lichen_alloc(meshcore_self(), sizeof(*mem), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!mem) {
        ESP_LOGE(TAG, "no memory for the air log");
        mc_list_label(screen, "Not enough memory to show the air log.", THEME_DIM);
        return screen;
    }
    list      = (mc_list_t){.keys = mem->keys, .items = mem->items, .cap = SHOW_ENTRIES};
    list.list = mc_list_screen(screen, "Air log", "Newest first. Headers only: message text is never kept.", "Up/Down",
                               "scroll");
    lv_obj_add_event_cb(list.list, on_key, LV_EVENT_KEY, NULL);
    shown_seq      = UINT32_MAX;
    sel_valid      = false;
    last_rebuild_s = mesh_uptime_s();
    timer          = lv_timer_create(refresh, 1000, NULL);
    refresh(NULL);
    return screen;
}

void meshcore_airlog_open(void *arg) {
    (void)arg;
    shell_open(build, SCREEN_MC_AIRLOG);
}
