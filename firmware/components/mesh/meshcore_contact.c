// A contact's details and actions (#49): see meshcore_contact.h. Everything with an lv_obj_t runs in the LVGL task;
// MeshCore is read through mesh_contact_detail() copies and changed through mesh_contact_do(), which only queues the
// change for MyMesh (it applies and saves it in its own loop, as for a phone app's command).
#include "meshcore_contact.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh_contact_fmt.h"
#include "meshcore_chat.h"
#include "theme.h"

#include <stdio.h>

#include <string.h>

static char const TAG[] = "meshcore";

#define SCREEN_CONTACT 160
#define SCREEN_SHARE   161
#define TICK_MS        1000
#define TICK_WAIT_MS   20  // the refresh: a busy MeshCore (saving files) skips a tick, the keys never wait on it
#define KEY_WAIT_MS    100 // a key press, in all (action and the copy after it): a short wait beats "busy"
#define REMOVE_ASK_S   20  // the careful Remove dialog cancels by itself after this
#define ADV_TYPE_CHAT  1   // MeshCore: a person's node (repeaters, rooms and sensors have no DMs)

typedef struct {
    mesh_contact_detail_t d;
    mesh_contact_line_t   lines[MESH_CONTACT_LINES];
    char                  uri[MESH_URI_MAX];
} contact_mem_t;

static uint8_t        prefix[6];
static char           title[MESH_NAME_MAX + 1];
static uint8_t        type;
static lv_obj_t      *list_screen; // the screen the details were opened from: Remove goes back to it
static contact_mem_t *mem;         // PSRAM, while the details screen exists
static bool           have_detail, gone;
static int            shown_path_len, shown_flags; // what the rows show; -1: nothing yet
static lv_obj_t      *info[MESH_CONTACT_LINES], *note, *fav_row, *reset_row;
static lv_timer_t    *timer;

static void set_text(lv_obj_t *label, char const *text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text); // only on a change: the same text every second would redraw for nothing
    }
}

static void set_note(lv_obj_t *label, char const *text, bool error) {
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(error ? THEME_RED : THEME_TEAL), 0);
        set_text(label, text);
    }
}

static void mark_gone(void) {
    gone = true;
    set_note(note, "This contact is no longer in MeshCore's list", true);
}

// The copy under the mesh lock, the widgets after it
static void fill(uint32_t wait_ms) {
    if (gone || !mem) {
        return;
    }
    size_t r = mesh_contact_detail(prefix, &mem->d, wait_ms);
    if (r == MESH_BUSY) {
        return;
    }
    if (r == 0) {
        mark_gone();
        return;
    }
    have_detail = true;
    size_t n    = mesh_contact_lines(&mem->d, mem->lines, MESH_CONTACT_LINES);
    for (size_t i = 0; i < n; i++) {
        set_text(info[i], mem->lines[i].text);
    }
    if (mem->d.path_len != shown_path_len || mem->d.flags != shown_flags) { // the rows redraw only on a change
        char hops[16];
        mesh_format_hops(mem->d.path_len, hops, sizeof(hops));
        menu_set_value(reset_row, hops);
        menu_set_value(fav_row, mem->d.flags & MESH_CONTACT_FAV ? "on" : "off");
        shown_path_len = mem->d.path_len;
        shown_flags    = mem->d.flags;
    }
}

static void tick(lv_timer_t *t) {
    (void)t;
    fill(TICK_WAIT_MS);
}

// An action, then the copy again so the rows show it. MyMesh saves the contacts itself a few seconds later.
static bool act(mesh_contact_op_t op, lv_obj_t *status) {
    if (gone) {
        return false;
    }
    uint32_t start = lv_tick_get();
    size_t   r     = have_detail ? mesh_contact_do(prefix, op, KEY_WAIT_MS) : MESH_BUSY;
    if (r == MESH_BUSY) {
        set_note(status, "MeshCore is busy, try again", true);
        return false;
    }
    if (r == 0) {
        mark_gone();
        return false;
    }
    uint32_t spent = lv_tick_elaps(start);
    if (spent < KEY_WAIT_MS) {
        fill(KEY_WAIT_MS - spent); // else the next tick shows it: the keys never wait more than KEY_WAIT_MS in all
    }
    return true;
}

static void on_message(void *arg) {
    (void)arg;
    meshcore_chat_open_dm(prefix, title[0] ? title : "(no name)");
}

static void on_reset(void *arg) {
    (void)arg;
    if (act(MESH_CO_RESET_PATH, note)) {
        set_note(note, "Path reset: the next message floods and finds a new one", false);
    }
}

static void on_fav(void *arg) {
    (void)arg;
    bool on = !(mem && (mem->d.flags & MESH_CONTACT_FAV));
    if (act(on ? MESH_CO_FAV_ON : MESH_CO_FAV_OFF, note)) {
        set_note(note, on ? "Favourite: first in the Contacts list" : "No longer a favourite", false);
    }
}

// ---- Share: the link as text (sending the contact on air is not done yet) ------------------------------------------

static lv_obj_t *add_text(lv_obj_t *menu, char const *text) {
    lv_obj_t *label = menu_note(menu, text);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_TEXT), 0);
    return label;
}

static lv_obj_t *build_share(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Share contact");
    if (!mem || !have_detail || !mesh_contact_uri(mem->d.name, mem->d.pub, mem->d.type, mem->uri, sizeof(mem->uri))) {
        menu_note(menu, "No link: MeshCore was busy, press Esc and try again");
        return screen;
    }
    menu_note(menu, "Link for MeshCore apps (Add contact, paste):");
    add_text(menu, mem->uri);
    menu_note(menu, "Public key:");
    add_text(menu, mem->lines[MESH_LINE_KEY].text + strlen("Key: "));
    menu_note(menu, "Esc: back");
    ESP_LOGW(TAG, "share %s", mem->uri); // on the console too, to copy it from there
    return screen;
}

static void on_share(void *arg) {
    (void)arg;
    shell_open(build_share, SCREEN_SHARE);
}

// ---- Remove: a careful dialog, so a held Enter (LVGL repeats it) never confirms it --------------------------------

static void remove_answer(bool yes) {
    if (!yes || !mem) { // no, timed out, or the details screen went away meanwhile
        return;
    }
    if (act(MESH_CO_REMOVE, note)) {
        shell_back_to(list_screen, NULL, NULL); // past the details: the contact is gone
    }
}

static void on_remove(void *arg) {
    (void)arg;
    char                 text[MESH_NAME_MAX + 64];
    shell_dialog_t const d = {
        .title        = text,
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: remove · Esc: keep it",
        .seconds      = REMOVE_ASK_S,
        .careful      = true,
    };
    snprintf(text, sizeof(text), "Remove %s from MeshCore? The chat stays.", title[0] ? title : "(no name)");
    if (!shell_dialog_countdown(&d, remove_answer)) {
        set_note(note, "Could not ask, try again", true);
    }
}

// ---- the details screen ---------------------------------------------------------------------------------------------

static void on_delete(lv_event_t *e) {
    (void)e;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    heap_caps_free(mem);
    mem = NULL;
    memset(info, 0, sizeof(info));
    note = fav_row = reset_row = NULL;
}

static lv_obj_t *build_detail(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, title[0] ? title : "(no name)");
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    heap_caps_free(mem);
    mem            = heap_caps_calloc(1, sizeof(*mem), MALLOC_CAP_SPIRAM);
    have_detail    = false;
    gone           = false;
    shown_path_len = shown_flags = -1;
    if (!mem) {
        menu_note(menu, "Not enough memory to show the contact.");
        return screen;
    }
    for (size_t i = 0; i < MESH_CONTACT_LINES; i++) {
        info[i] = add_text(menu, "...");
    }
    if (type == ADV_TYPE_CHAT) {
        menu_add(menu, "Message", "", on_message, NULL); // the first row: Enter Enter is the old "message a contact"
    }
    reset_row = menu_add(menu, "Reset path", "", on_reset, NULL);
    fav_row   = menu_add(menu, "Favourite", "", on_fav, NULL);
    menu_add(menu, "Share", "link", on_share, NULL);
    menu_add(menu, "Remove", "", on_remove, NULL);
    note = menu_note(menu, "");
    fill(KEY_WAIT_MS);
    timer = lv_timer_create(tick, TICK_MS, NULL);
    return screen;
}

void meshcore_contact_open(mesh_contact_t const *c) {
    memcpy(prefix, c->pubkey, sizeof(prefix));
    snprintf(title, sizeof(title), "%s", c->name);
    type        = c->type;
    list_screen = lv_screen_active();
    shell_open(build_detail, SCREEN_CONTACT);
}
