// Request telemetry screen (#162): see meshcore_telem.h. Everything with an lv_obj_t runs in the LVGL task; MeshCore is
// only reached through mesh_remote_start / mesh_remote_get, which never wait for it. The text is mesh_telem.h.
#include "meshcore_telem.h"
#include "screen_ids.h"

#include "lichen_mem.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_telem.h"
#include "meshcore_run.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define POLL_MS 250

typedef struct {
    rr_t rr;
    char text[TL_LINES_MAX * TL_LINE_MAX + 160];
} view_mem_t;

static uint8_t     target[6];
static char        title[MESH_NAME_MAX + 1];
static view_mem_t *mem;        // PSRAM, while the page exists
static lv_obj_t   *screen_obj; // the page that owns mem and timer
static lv_timer_t *timer;
static lv_obj_t   *result, *again, *note;
static uint32_t    seq; // the request this page made; 0 when it could not start
static bool        shown_busy;

static void set_text(lv_obj_t *label, char const *text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text); // only on a change: the same text every tick would redraw for nothing
    }
}

static void on_delete(lv_event_t *e) {
    if (lv_event_get_target(e) != screen_obj) {
        return; // a late delete of an older page must not free what a newer one owns
    }
    screen_obj = NULL;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    lichen_free(meshcore_ctx(), mem);
    mem    = NULL;
    result = again = note = NULL;
}

static void show(void) {
    if (!mem || !result || !seq || !mesh_remote_get(&mem->rr)) {
        return;
    }
    rr_t const *r = &mem->rr;
    if (r->seq != seq) {
        set_text(result, "Another request to a contact was started: Again asks for telemetry once more.");
        return;
    }
    tl_view_text(r, target, mem->text, sizeof(mem->text));
    set_text(result, mem->text);
    bool busy = rr_busy(r);
    if (busy != shown_busy) {
        menu_set_value(again, busy ? "..." : "");
        shown_busy = busy;
    }
}

static void tick(lv_timer_t *t) {
    (void)t;
    show();
}

// The one place a request leaves: called after the person's confirm only
static void start_request(void) {
    char why[96];
    seq = mesh_telemetry_start(target, MESH_FROM_SCREEN, why, sizeof(why));
    if (!seq) {
        char text[128];
        snprintf(text, sizeof(text), "Not sent: %s", why);
        set_text(note, text);
        return;
    }
    set_text(note, "");
    show();
}

static void again_row(void *arg);

static lv_obj_t *build_page(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create_named(screen, "Telemetry", title[0] ? title : "(no name)");
    lichen_ui_event_add(screen, on_delete, LV_EVENT_DELETE, NULL);
    screen_obj = screen;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    lichen_free(meshcore_ctx(), mem);
    mem        = lichen_alloc(meshcore_ctx(), sizeof(*mem), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    seq        = 0;
    shown_busy = false;
    if (!mem) {
        menu_note(menu, "Not enough memory.");
        return screen;
    }
    again = menu_add(menu, "Again", "", again_row, NULL);
    note  = menu_note(menu, "");
    result = menu_note(menu, "Asking...");
    lv_obj_set_style_text_color(result, lv_color_hex(THEME_TEXT), 0);
    timer = lichen_ui_timer_create(tick, POLL_MS, NULL);
    // Normally a new page asks as it is built. If a request to this contact is running already, adopt it instead.
    if (mesh_remote_get(&mem->rr) && mem->rr.op == RR_TELEMETRY && rr_busy(&mem->rr) &&
        memcmp(mem->rr.prefix, target, sizeof(target)) == 0) {
        seq = mem->rr.seq;
        show();
    } else {
        start_request();
    }
    return screen;
}

static void confirmed(bool yes) {
    if (!yes) {
        return;
    }
    uint32_t key = shell_key_hash(target, sizeof(target));
    if (shell_screen_open(SCREEN_TELEMETRY, key)) {
        if (!(mem && seq && mesh_remote_get(&mem->rr) && mem->rr.seq == seq && rr_busy(&mem->rr))) {
            start_request(); // the page is open already (Again): this Enter is the go-ahead
        }
    }
    shell_open_keyed(build_page, SCREEN_TELEMETRY, key); // a new page sends its request as it is built
}

uint32_t meshcore_telem_ask(uint8_t const prefix[6], char const *name) {
    memcpy(target, prefix, sizeof(target));
    snprintf(title, sizeof(title), "%s", name);
    char     t[MESH_NAME_MAX + 40];
    uint32_t wait = mesh_telemetry_wait_s(target);
    if (wait) {
        return wait; // the caller says so on its own page
    }
    snprintf(t, sizeof(t), "Ask %s for its telemetry?", title[0] ? title : "(no name)");
    shell_dialog_t const d = {
        .title         = t,
        .count_prefix  = "This sends a request on air. A contact answers only if it shares telemetry with you. (",
        .seconds       = 30,
        .careful       = true,
        .confirm_label = "send",
    };
    if (!shell_dialog_countdown(&d, confirmed)) {
        set_text(note, mc_busy_note());
    }
    return 0;
}

static void again_row(void *arg) {
    (void)arg;
    if (mem && seq && mesh_remote_get(&mem->rr) && mem->rr.seq == seq && rr_busy(&mem->rr)) {
        return; // still waiting for this one: a held Enter does not ask more
    }
    uint32_t wait = meshcore_telem_ask(target, title);
    if (wait) {
        char t[24];
        snprintf(t, sizeof(t), "Wait %lu s", (unsigned long)wait);
        set_text(note, t);
    }
}
