// A contact's details and actions (#49): see meshcore_contact.h. Everything with an lv_obj_t runs in the LVGL task;
// MeshCore is read through mesh_contact_detail() copies and changed through mesh_contact_do(), which only queues the
// change for MyMesh (it applies and saves it in its own loop, as for a phone app's command).
#include "meshcore_contact.h"
#include "screen_ids.h"

#include "esp_heap_caps.h"
#include "mesh_wipe.h"
#include "esp_log.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh_contact_fmt.h"
#include "mesh.h"
#include "mesh_format.h"
#include "meshcore_chat.h"
#include "meshcore_remote.h"
#include "meshcore_run.h"
#include "theme.h"

#include <stdio.h>

#include <string.h>

static char const TAG[] = "meshcore";

#define TRACE_TICK_MS  250
#define TICK_MS        1000
#define TICK_WAIT_MS   20  // the refresh: a busy MeshCore (saving files) skips a tick, the keys never wait on it
#define KEY_WAIT_MS    100 // a key press, in all (action and the copy after it): a short wait beats "busy"
#define ADV_TYPE_CHAT  1   // MeshCore: a person's node (repeaters, rooms and sensors have no DMs)

#define PREFIX_LEN 6

// One details screen's state (#149, #148): made in build_detail, hung on the screen's user_data, freed in on_delete
// (timer first). Every row callback and the timer get it as their arg, so a second contact page on the stack, a trace or
// a share screen never reads another page's contact. One PSRAM block.
typedef struct {
    mesh_contact_detail_t d;
    mesh_contact_line_t   lines[MESH_CONTACT_LINES];
    char                  uri[MESH_URI_MAX];
    rr_head_t             rr; // the login state, for the Log in row's value (#51)
    uint8_t               prefix[PREFIX_LEN];
    char                  title[MESH_NAME_MAX + 1];
    uint8_t               type;
    lv_obj_t             *list_screen; // the screen the details were opened from: Remove goes back to it
    bool                  have_detail, gone;
    int                   shown_path_len, shown_flags, shown_login; // what the rows show; -1: nothing yet
    lv_obj_t             *info[MESH_CONTACT_LINES], *note, *fav_row, *reset_row, *login_row;
    lv_timer_t           *timer;
} contact_t;

// What meshcore_contact_open wants built: read once by build_detail, which runs inside shell_open_keyed (build takes no
// arguments) under the LVGL lock
static struct {
    uint8_t    prefix[PREFIX_LEN];
    char       name[MESH_NAME_MAX + 1];
    uint8_t    type;
    lv_obj_t  *list_screen;
} want_contact;

// The same for the trace and share screens: the details screen they were opened from
static contact_t *want_ct;

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

static void mark_gone(contact_t *ct) {
    ct->gone = true;
    set_note(ct->note, "This contact is no longer in MeshCore's list", true);
}

// The copy under the mesh lock, the widgets after it
static void fill(contact_t *ct, uint32_t wait_ms) {
    if (ct->gone) {
        return;
    }
    size_t r = mesh_contact_detail(ct->prefix, &ct->d, wait_ms);
    if (r == MESH_BUSY) {
        return;
    }
    if (r == 0) {
        mark_gone(ct);
        return;
    }
    ct->have_detail = true;
    size_t n        = mesh_contact_lines(&ct->d, ct->lines, MESH_CONTACT_LINES);
    for (size_t i = 0; i < n; i++) {
        set_text(ct->info[i], ct->lines[i].text);
    }
    if (ct->d.path_len != ct->shown_path_len || ct->d.flags != ct->shown_flags) { // the rows redraw only on a change
        char hops[16];
        mesh_format_hops(ct->d.path_len, hops, sizeof(hops));
        menu_set_value(ct->reset_row, hops);
        menu_set_on(ct->fav_row, (ct->d.flags & MESH_CONTACT_FAV) != 0);
        ct->shown_path_len = ct->d.path_len;
        ct->shown_flags    = ct->d.flags;
    }
    if (ct->login_row && mesh_remote_head(&ct->rr)) { // the small copy: this runs every second (#117)
        int in = rr_head_logged_in(&ct->rr, ct->prefix) ? 1 + ct->rr.admin : 0;
        if (in != ct->shown_login) {
            menu_set_value(ct->login_row, in == 2 ? "Admin" : in ? "Logged in" : "");
            ct->shown_login = in;
        }
    }
}

static void tick(lv_timer_t *t) {
    fill(lv_timer_get_user_data(t), TICK_WAIT_MS);
}

// An action, then the copy again so the rows show it. MyMesh saves the contacts itself a few seconds later.
static bool act(contact_t *ct, mesh_contact_op_t op) {
    lv_obj_t *status = ct->note;
    if (ct->gone) {
        return false;
    }
    uint32_t start = lv_tick_get();
    size_t   r     = ct->have_detail ? mesh_contact_do(ct->prefix, op, KEY_WAIT_MS) : MESH_BUSY;
    if (r == MESH_BUSY) {
        set_note(status, mc_busy_note(), true);
        return false;
    }
    if (r == 0) {
        mark_gone(ct);
        return false;
    }
    uint32_t spent = lv_tick_elaps(start);
    if (spent < KEY_WAIT_MS) {
        fill(ct, KEY_WAIT_MS - spent); // else the next tick shows it: the keys never wait more than KEY_WAIT_MS in all
    }
    return true;
}

static void on_message(void *arg) {
    contact_t *ct = arg;
    meshcore_chat_open_dm(ct->prefix, ct->title[0] ? ct->title : "(no name)");
}

static void on_reset(void *arg) {
    contact_t *ct = arg;
    if (act(ct, MESH_CO_RESET_PATH)) {
        set_note(ct->note, "Path reset: the next message floods and finds a new one", false);
    }
}

static void on_fav(void *arg) {
    contact_t *ct = arg;
    bool on = !(ct->d.flags & MESH_CONTACT_FAV);
    if (act(ct, on ? MESH_CO_FAV_ON : MESH_CO_FAV_OFF)) {
        set_note(ct->note, on ? "Favourite: first in the Contacts list" : "No longer a favourite", false);
    }
}

// ---- Repeaters and rooms (#51): the screens are meshcore_remote.c, each starts only on an Enter of the person ---------

static void on_login(void *arg) {
    contact_t *ct = arg;
    meshcore_remote_login(ct->prefix, ct->title);
}

static void on_status(void *arg) {
    contact_t *ct = arg;
    meshcore_remote_status(ct->prefix, ct->title);
}

static void on_cli(void *arg) {
    contact_t *ct = arg;
    meshcore_remote_cli(ct->prefix, ct->title);
}

static void on_settings(void *arg) {
    contact_t *ct = arg;
    meshcore_remote_settings(ct->prefix, ct->title);
}

static void on_posts(void *arg) {
    contact_t *ct = arg;
    meshcore_chat_open_dm(ct->prefix, ct->title[0] ? ct->title : "(no name)");
}

static void on_forget(void *arg) {
    contact_t *ct = arg;
    char saved[MF_PASSWORD_MAX + 2];
    bool had = meshcore_pw_get(ct->prefix, saved, sizeof(saved));
    mesh_wipe(saved, sizeof(saved));
    meshcore_pw_forget(ct->prefix);
    set_note(ct->note, had ? "Password forgotten" : "No password was remembered", false);
}

static void on_logout(void *arg) {
    contact_t *ct = arg;
    if (mesh_remote_logout(ct->prefix, KEY_WAIT_MS)) {
        set_note(ct->note, "Logged out: MeshCore stops its keep-alives to it", false);
        fill(ct, 0);
    } else {
        set_note(ct->note, mc_busy_note(), true);
    }
}

// ---- Trace path and Discover path (#53): the request goes to the mesh task, this screen polls its copy -----------------

// One trace/discovery result screen's state (#149): made in build, hung on the screen's user_data, freed in on_delete
// (timer first). Another trace of the same contact with the other kind is a separate screen on the stack and must not
// share any of it. One PSRAM block; the mt_t is the big part.
typedef struct {
    mt_t       t;
    mt_line_t  lines[MT_LINES_MAX];
    char       text[MT_LINES_MAX * MT_LINE_MAX];
    mt_kind_t  kind;
    uint8_t    prefix[PREFIX_LEN]; // the contact this screen is about
    uint32_t   seq;                    // ours; 0 when it could not start
    lv_obj_t  *label, *again;
    lv_timer_t *timer;
    bool       shown_busy;
} trace_t;

static lv_obj_t *add_text(lv_obj_t *menu, char const *text);

// What open_trace wants built: read once, by build_trace, which runs inside shell_open_keyed under the LVGL lock
static mt_kind_t want_kind;

static void trace_show(trace_t *tr) {
    if (!tr->label || !tr->seq || !mesh_trace_get(&tr->t)) {
        return;
    }
    mt_t const *t = &tr->t;
    if (t->seq != tr->seq) {
        set_text(tr->label, "Another trace or discovery was started (the console?): Enter starts this one again");
        return;
    }
    size_t n   = mt_format(t, tr->lines, MT_LINES_MAX);
    size_t at  = 0;
    size_t cap = sizeof(tr->text);
    tr->text[0] = 0;
    for (size_t i = 0; i < n && at + 1 < cap; i++) {
        at += (size_t)snprintf(tr->text + at, cap - at, "%s%s", i ? "\n" : "", tr->lines[i].text);
    }
    set_text(tr->label, tr->text);
    bool busy = mt_busy(t);
    if (busy != tr->shown_busy) { // the row redraws only on a change
        menu_set_value(tr->again, busy ? "..." : "");
        tr->shown_busy = busy;
    }
}

static void trace_tick(lv_timer_t *timer) {
    trace_show(lv_timer_get_user_data(timer));
}

static void trace_start(trace_t *tr) {
    char const *why = NULL;
    tr->seq         = mesh_trace_start(tr->prefix, tr->kind, &why);
    if (!tr->seq) {
        char text[96];
        snprintf(text, sizeof(text), "Not started: %s", why ? why : "?");
        set_text(tr->label, text);
        return;
    }
    trace_show(tr);
}

static void on_trace_again(void *arg) {
    trace_t *tr = arg;
    if (tr->seq && mesh_trace_get(&tr->t) && tr->t.seq == tr->seq && mt_busy(&tr->t)) {
        return; // still waiting for this one: a held Enter does not queue more
    }
    trace_start(tr);
}

static void on_trace_delete(lv_event_t *e) {
    trace_t *tr = lv_obj_get_user_data(lv_event_get_current_target(e));
    if (!tr) {
        return;
    }
    if (tr->timer) {
        lv_timer_delete(tr->timer);
    }
    heap_caps_free(tr);
}

static lv_obj_t *build_trace(void) {
    contact_t *ct    = want_ct;
    mt_kind_t  kind  = want_kind;
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu = menu_create_named(screen, kind == MT_TRACE ? "Trace path" : "Discover path",
                                       ct->title[0] ? ct->title : "(no name)");
    trace_t *tr = heap_caps_calloc(1, sizeof(*tr), MALLOC_CAP_SPIRAM);
    if (!tr) {
        menu_note(menu, "Not enough memory.");
        return screen;
    }
    tr->kind = kind;
    memcpy(tr->prefix, ct->prefix, sizeof(tr->prefix));
    lv_obj_set_user_data(screen, tr);
    lv_obj_add_event_cb(screen, on_trace_delete, LV_EVENT_DELETE, NULL);
    menu_note(menu, kind == MT_TRACE ? "Along the stored path and back; SNR as each hop heard it:"
                                     : "Asks the contact over the whole mesh which path to use:");
    tr->label = add_text(menu, "Sending...");
    tr->again = menu_add(menu, "Again", "", on_trace_again, tr);
    trace_start(tr);
    tr->timer = lv_timer_create(trace_tick, TRACE_TICK_MS, tr);
    return screen;
}

static void open_trace(contact_t *ct, mt_kind_t kind) {
    if (ct->gone) {
        return;
    }
    want_kind = kind;
    want_ct   = ct;
    uint8_t id[PREFIX_LEN + 1];
    memcpy(id, ct->prefix, PREFIX_LEN);
    id[PREFIX_LEN] = (uint8_t)kind;
    shell_open_keyed(build_trace, SCREEN_TRACE, shell_key_hash(id, sizeof(id)));
}

static void on_trace(void *arg) {
    open_trace(arg, MT_TRACE);
}

static void on_discover(void *arg) {
    open_trace(arg, MT_DISCOVER);
}

// ---- Share: the link as text (sending the contact on air is not done yet) ------------------------------------------

static lv_obj_t *add_text(lv_obj_t *menu, char const *text) {
    lv_obj_t *label = menu_note(menu, text);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_TEXT), 0);
    return label;
}

static lv_obj_t *build_share(void) {
    contact_t *ct    = want_ct; // the details screen is open under this one, so it lives as long as this build
    lv_obj_t  *screen = shell_page_create();
    lv_obj_t  *menu   = menu_create_named(screen, "Share contact", ct->title[0] ? ct->title : "(no name)");
    if (!ct->have_detail || !mesh_contact_uri(ct->d.name, ct->d.pub, ct->d.type, ct->uri, sizeof(ct->uri))) {
        menu_note(menu, mc_busy_note());
        return screen;
    }
    menu_note(menu, "Link for MeshCore apps (Add contact, paste):");
    add_text(menu, ct->uri);
    menu_note(menu, "Public key:");
    add_text(menu, ct->lines[MESH_LINE_KEY].text + strlen("Key: "));
    ESP_LOGI(TAG, "share %s", ct->uri); // on the console too, to copy it from there
    return screen;
}

static void on_share(void *arg) {
    contact_t *ct = arg;
    want_ct       = ct;
    shell_open_keyed(build_share, SCREEN_SHARE, shell_key_hash(ct->prefix, PREFIX_LEN));
}

// ---- Remove and Reset path: DANGER rows, the careful dialog is the row's own (menu_set_danger) -----------------------

static void on_remove(void *arg) {
    contact_t *ct = arg;
    if (!act(ct, MESH_CO_REMOVE)) { // MeshCore did not take it
        return;
    }
    meshcore_pw_forget(ct->prefix); // a remembered repeater/room password goes with the contact (#51)
    shell_back_to(ct->list_screen, NULL, NULL); // past the details: the contact is gone
}

// ---- the details screen ---------------------------------------------------------------------------------------------

static void on_delete(lv_event_t *e) {
    contact_t *ct = lv_obj_get_user_data(lv_event_get_current_target(e));
    if (!ct) {
        return;
    }
    if (ct->timer) {
        lv_timer_delete(ct->timer);
    }
    heap_caps_free(ct);
}

static void add_remote_rows(lv_obj_t *menu, contact_t *ct) {
    uint8_t type = ct->type;
    ct->login_row = menu_add(menu, "Log in", "", on_login, ct);
    menu_set_kind(ct->login_row, MENU_OPEN);
    if (type == MESH_ADV_TYPE_ROOM) {
        menu_set_kind(menu_add(menu, "Posts", "", on_posts, ct), MENU_OPEN); // the room's posts after login
    }
    menu_set_kind(menu_add(menu, "Status", "", on_status, ct), MENU_OPEN);
    menu_set_kind(menu_add(menu, "Command...", "", on_cli, ct), MENU_OPEN);
    if (type == MESH_ADV_TYPE_REPEATER) {
        menu_set_kind(menu_add(menu, "Settings", "", on_settings, ct), MENU_OPEN); // #117: needs the admin login
    }
    menu_add(menu, "Log out", "", on_logout, ct);
    lv_obj_t *forget = menu_add(menu, "Forget password", "", on_forget, ct);
    menu_set_danger(forget, "Forget the saved password of this repeater or room? Log in asks for it again.", "forget");
}

static lv_obj_t *build_detail(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create_named(screen, want_contact.type == ADV_TYPE_CHAT ? "Contact" : mesh_contact_type_name(want_contact.type),
                                       want_contact.name[0] ? want_contact.name : "(no name)");
    contact_t *ct = heap_caps_calloc(1, sizeof(*ct), MALLOC_CAP_SPIRAM);
    if (!ct) {
        menu_note(menu, "Not enough memory to show the contact.");
        return screen;
    }
    memcpy(ct->prefix, want_contact.prefix, PREFIX_LEN);
    snprintf(ct->title, sizeof(ct->title), "%s", want_contact.name);
    ct->type           = want_contact.type;
    ct->list_screen    = want_contact.list_screen;
    ct->shown_path_len = ct->shown_flags = ct->shown_login = -1;
    lv_obj_set_user_data(screen, ct);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    for (size_t i = 0; i < MESH_CONTACT_LINES; i++) {
        ct->info[i] = add_text(menu, "...");
    }
    if (ct->type == ADV_TYPE_CHAT) {
        menu_set_kind(menu_add(menu, "Message", "", on_message, ct), MENU_OPEN); // the first row: Enter Enter is the old "message a contact"
    }
    if (ct->type == MESH_ADV_TYPE_REPEATER || ct->type == MESH_ADV_TYPE_ROOM) {
        add_remote_rows(menu, ct);
    }
    ct->reset_row = menu_add(menu, "Reset path", "", on_reset, ct);
    menu_set_danger(ct->reset_row, "Reset the route to this contact? The next message floods to find a new one.", "reset");
    menu_set_kind(menu_add(menu, "Trace path", "", on_trace, ct), MENU_OPEN);
    menu_set_kind(menu_add(menu, "Discover path", "", on_discover, ct), MENU_OPEN);
    ct->fav_row = menu_add(menu, "Favourite", "", on_fav, ct);
    menu_set_kind(ct->fav_row, MENU_TOGGLE);
    menu_set_kind(menu_add(menu, "Share", "", on_share, ct), MENU_OPEN);
    lv_obj_t *remove_row = menu_add(menu, "Remove", "", on_remove, ct);
    char      ask[MESH_NAME_MAX + 64];
    snprintf(ask, sizeof(ask), "Remove %s from MeshCore? The chat stays.", ct->title[0] ? ct->title : "(no name)");
    menu_set_danger(remove_row, ask, "remove");
    ct->note = menu_note(menu, "");
    fill(ct, KEY_WAIT_MS);
    ct->timer = lv_timer_create(tick, TICK_MS, ct);
    return screen;
}

void meshcore_contact_open(mesh_contact_t const *c) {
    memcpy(want_contact.prefix, c->pubkey, PREFIX_LEN);
    snprintf(want_contact.name, sizeof(want_contact.name), "%s", c->name);
    want_contact.type        = c->type;
    want_contact.list_screen = lv_screen_active();
    // One page per contact; an open one is reused and build_detail does not run, so these are only read by a new page
    shell_open_keyed(build_detail, SCREEN_CONTACT, shell_key_hash(c->pubkey, PREFIX_LEN));
}
