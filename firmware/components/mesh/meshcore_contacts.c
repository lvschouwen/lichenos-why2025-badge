// The Contacts, Repeaters and Heard tiles' screen (#88): one contact list in the view's filter and order
// (mesh_contacts_view), from the old Contacts tab. Enter opens the highlighted contact's details (#49). The store keeps
// only contacts (MeshCore adds the nodes it hears unless manual add is on), so Heard is the contacts by last heard.
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lichen_ui.h"
#include "mesh.h"
#include "mesh_format.h"
#include "meshcore_contact.h"
#include "meshcore_list.h"
#include "meshcore_screens.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "meshcore";

#define SCREEN_CONTACTS 171
#define SHOW_CONTACTS   100
#define AGE_REFRESH_S   30
#define SCREEN_WAIT_MS  20

typedef struct {
    char const *title, *note, *empty;
} view_text_t;

static view_text_t const VIEWS[] = {
    [MESH_VIEW_ALL]       = {"Contacts", NULL, "No nodes heard yet."},
    [MESH_VIEW_REPEATERS] = {"Repeaters", "Repeaters and rooms among the contacts.", "No repeaters or rooms heard yet."},
    [MESH_VIEW_HEARD]     = {"Heard", "Newest first. Only nodes kept as contacts are listed: others are not stored.",
                             "No nodes heard yet."},
};

typedef struct {
    mesh_contact_t contacts[SHOW_CONTACTS];
    uint32_t       keys[SHOW_CONTACTS];
    scroll_item_t  items[SHOW_CONTACTS];
} contacts_mem_t;

static contacts_mem_t     *mem; // PSRAM, while the screen exists
static mc_list_t           list;
static mesh_contact_view_t view;
static lv_timer_t         *timer;
static lv_obj_t           *screen_obj;
static uint32_t            shown_sig, last_rebuild_s;
static uint8_t             sel_key[6]; // the highlighted contact (its key prefix), kept when the order changes
static bool                sel_valid;

// The highlight after the list changed: on the same contact if it is still there, else the same place
static void settle_selection(size_t count) {
    size_t at = list.sel < count ? list.sel : (count ? count - 1 : 0);
    for (size_t i = 0; sel_valid && i < count; i++) {
        if (memcmp(mem->contacts[i].pubkey, sel_key, sizeof(sel_key)) == 0) {
            at = i;
            break;
        }
    }
    list.sel  = at;
    sel_valid = count > 0;
    if (sel_valid) {
        memcpy(sel_key, mem->contacts[at].pubkey, sizeof(sel_key));
    }
}

static void add_contact_row(mesh_contact_t const *c, bool selected) {
    char age[24], hops[16], meta[80], name[MESH_NAME_MAX + 8];
    mesh_format_age(c->age_s, age, sizeof(age));
    mesh_format_hops(c->path_len, hops, sizeof(hops)); // the hash size bits are not hops (#85)
    snprintf(meta, sizeof(meta), "%s, %s, %s", mesh_type_name(c->type), hops, age);
    snprintf(name, sizeof(name), "%s%s", c->flags & MESH_CONTACT_FAV ? "⭐ " : "", // a favourite (#49)
             c->name[0] ? c->name : "(no name)");
    lv_obj_t *row = mc_list_row(&list, selected);
    mc_list_label(row, name, THEME_TEXT);
    mc_list_label(row, meta, THEME_DIM);
}

// Only the first SHOW_CONTACTS fit: say so under them rather than let a long list look complete
static void add_more_note(size_t count, size_t total) {
    if (total > count) {
        char text[64];
        snprintf(text, sizeof(text), "showing %u of %u", (unsigned)count, (unsigned)total);
        mc_list_label(list.list, text, THEME_DIM);
    }
}

static void rebuild(size_t count, size_t total) {
    mc_scroll_t s = mc_list_save(&list);
    lv_obj_clean(list.list);
    list.n = count;
    settle_selection(count);
    if (!count) {
        mc_list_label(list.list, mesh_running() ? VIEWS[view].empty : "MeshCore is not running.", THEME_DIM);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        list.keys[i] = scroll_key_hash(mem->contacts[i].pubkey, sizeof(mem->contacts[i].pubkey), 0);
        add_contact_row(&mem->contacts[i], i == list.sel);
    }
    add_more_note(count, total);
    mc_list_restore(&list, s);
}

// Changes when the count changes, someone is heard (again): the newest hearing time, or a contact's favourite flag
// or path changes, or the order does
static uint32_t signature(size_t n, size_t total) {
    uint32_t newest = UINT32_MAX;
    uint32_t h      = scroll_key_hash(&total, sizeof(total), 0);
    for (size_t i = 0; i < n; i++) {
        mesh_contact_t const *c = &mem->contacts[i];
        newest                  = c->age_s < newest ? c->age_s : newest;
        h                       = scroll_key_hash(c->pubkey, sizeof(c->pubkey), h);
        h                       = scroll_key_hash(&c->flags, 1, h);
        h                       = scroll_key_hash(&c->path_len, 1, h);
    }
    uint32_t heard_at = n ? mesh_uptime_s() - newest : 0;
    return h ^ heard_at;
}

static void refresh(lv_timer_t *t) {
    if (t && lv_screen_active() != screen_obj) {
        return; // the details are open on top: no mesh lock for rows nobody sees
    }
    size_t total;
    size_t count = mesh_contacts_view(view, mem->contacts, SHOW_CONTACTS, &total, SCREEN_WAIT_MS);
    if (count == MESH_BUSY) {
        return; // MeshCore is busy (saving files): the screen stays as it is until the next tick
    }
    uint32_t sig   = signature(count, total);
    bool     stale = mesh_uptime_s() - last_rebuild_s >= AGE_REFRESH_S;
    if (sig != shown_sig || stale) {
        rebuild(count, total);
        shown_sig      = sig;
        last_rebuild_s = mesh_uptime_s();
    }
}

// By its key, not the row: refresh() takes a new copy every tick, a rebuild may not have followed yet
static void open_selected(void) {
    for (size_t i = 0; sel_valid && i < list.n; i++) {
        if (memcmp(mem->contacts[i].pubkey, sel_key, sizeof(sel_key)) == 0) {
            meshcore_contact_open(&mem->contacts[i]);
            return;
        }
    }
}

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        if (mc_list_move(&list, key == LV_KEY_UP ? -1 : 1)) {
            memcpy(sel_key, mem->contacts[list.sel].pubkey, sizeof(sel_key));
        }
    } else if (key == LV_KEY_ENTER) {
        open_selected(); // the details (#49): Message is their first row for a companion
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    heap_caps_free(mem);
    mem        = NULL;
    screen_obj = NULL;
    list       = (mc_list_t){0};
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    screen_obj       = screen;
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    mem = heap_caps_calloc(1, sizeof(*mem), MALLOC_CAP_SPIRAM);
    if (!mem) {
        ESP_LOGE(TAG, "no memory for the contacts");
        mc_list_label(screen, "Not enough memory to show the contacts.", THEME_DIM);
        return screen;
    }
    list      = (mc_list_t){.keys = mem->keys, .items = mem->items, .cap = SHOW_CONTACTS};
    list.list = mc_list_screen(screen, VIEWS[view].title, VIEWS[view].note, "Up/Down: choose    Enter: details");
    lv_obj_add_event_cb(list.list, on_key, LV_EVENT_KEY, NULL);
    shown_sig      = UINT32_MAX;
    sel_valid      = false;
    last_rebuild_s = mesh_uptime_s();
    timer          = lv_timer_create(refresh, 1000, NULL);
    refresh(NULL); // not loaded yet: the rows right away all the same
    return screen;
}

void meshcore_contacts_open(mesh_contact_view_t v) {
    view = v <= MESH_VIEW_HEARD ? v : MESH_VIEW_ALL;
    shell_open(build, SCREEN_CONTACTS);
}
