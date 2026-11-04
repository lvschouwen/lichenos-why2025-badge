// The MeshCore app's Discover nearby page (#167): one zero-hop request, then a live list of who answered (name or hash,
// type, SNR, age), newest first. Opening the page sends nothing: the first row asks, behind a careful confirm. After the
// listening window the status says "Heard N" or that nobody answered, and the first row asks again (not within a minute).
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

#define ASK_CONFIRM_S 20
#define TICK_MS       1000
#define AGE_REFRESH_S 5

typedef struct {
    disc_t        d;
    disc_answer_t shown[DISC_MAX];
    uint32_t      keys[DISC_MAX + 1];
    scroll_item_t items[DISC_MAX + 1];
} discover_mem_t;

static discover_mem_t *mem; // PSRAM, while the screen exists
static mc_list_t       list;
static lv_timer_t     *timer;
static lv_obj_t       *screen_obj, *status_label;
static char const     *refusal;  // why the last ask did nothing, shown until the state changes
static uint32_t        sig_seen; // what the rows were built from
static uint32_t        built_s;
static bool            have_action_row;

// A cheap fingerprint of what the rows show; the ages move on a timer instead
static uint32_t signature(disc_t const *d, uint32_t wait_s) {
    uint32_t h = (uint32_t)d->state * 7919u + (uint32_t)d->count * 104729u + (wait_s ? 1u : 0u);
    for (size_t i = 0; i < d->count; i++) {
        h = h * 31u + d->a[i].at_s + (uint32_t)(uint16_t)d->a[i].snr_x10;
    }
    return h;
}

static bool action_row_wanted(disc_t const *d) {
    return d->state != DISC_PENDING && d->state != DISC_LISTENING;
}

static void add_action_row(disc_t const *d, uint32_t wait_s) {
    char      detail[64];
    bool      first = d->state == DISC_IDLE;
    lv_obj_t *row   = mc_list_row(&list, list.sel == 0);
    mc_list_line(row, first ? "Send request" : "Ask again", wait_s ? THEME_DIM : THEME_AMBER);
    if (wait_s) { // the intro under the heading says what sending does: no second line otherwise
        snprintf(detail, sizeof(detail), "Wait %u s before asking again", (unsigned)wait_s);
        mc_list_line(row, detail, THEME_DIM);
    }
}

static void add_answer_row(disc_answer_t const *a, uint32_t now_s, bool selected) {
    char title[64], detail[96];
    disc_row_title(a, title, sizeof(title));
    disc_row_detail(a, now_s, detail, sizeof(detail));
    lv_obj_t *row = mc_list_row(&list, selected);
    lv_obj_set_width(mc_list_line(row, title, THEME_TEXT), LV_PCT(100)); // full width: DOT cuts a long name
    lv_obj_set_width(mc_list_line(row, detail, THEME_DIM), LV_PCT(100));
}

static void rebuild(size_t answers, uint32_t now_s, uint32_t wait_s) {
    mc_scroll_t s = mc_list_save(&list);
    lv_obj_clean(list.list);
    have_action_row = action_row_wanted(&mem->d);
    size_t rows     = answers + (have_action_row ? 1 : 0);
    list.n          = rows;
    if (list.sel >= rows) {
        list.sel = rows ? rows - 1 : 0;
    }
    size_t at = 0;
    if (have_action_row) {
        mem->keys[at++] = 1;
        add_action_row(&mem->d, wait_s);
    }
    for (size_t i = 0; i < answers; i++) {
        mem->keys[at++] = scroll_key_hash(mem->shown[i].prefix, DISC_PREFIX, 2);
        add_answer_row(&mem->shown[i], now_s, at - 1 == list.sel);
    }
    if (have_action_row && mem->d.state == DISC_IDLE) {
        shell_page_hint(screen_obj, "Enter", "send", "Up/Down", "scroll", NULL);
    } else if (have_action_row) {
        shell_page_hint(screen_obj, "Enter", "ask again", "Up/Down", "scroll", NULL);
    } else {
        shell_page_hint(screen_obj, "Up/Down", "scroll", NULL);
    }
    if (!answers && mem->d.state == DISC_LISTENING) {
        mc_list_label(list.list, "Nobody has answered yet.", THEME_DIM);
    }
    mc_list_restore(&list, s);
}

static void refresh(lv_timer_t *t) {
    if (t && lv_screen_active() != screen_obj) {
        return;
    }
    mesh_discover_get(&mem->d);
    uint32_t now_s  = mesh_uptime_s();
    uint32_t wait_s = disc_wait_s(&mem->d, now_s);
    char     text[96];
    if (refusal && mem->d.state == DISC_IDLE) {
        snprintf(text, sizeof(text), "%s", refusal);
    } else {
        disc_status(&mem->d, now_s, text, sizeof(text));
    }
    lv_label_set_text(status_label, text);
    uint32_t sig = signature(&mem->d, wait_s);
    if (sig != sig_seen || now_s - built_s >= AGE_REFRESH_S || !t) {
        size_t n = disc_copy(&mem->d, mem->shown, DISC_MAX);
        rebuild(n, now_s, wait_s);
        sig_seen = sig;
        built_s  = now_s;
    }
}

static void ask_answer(bool yes) {
    if (!yes || !mem) {
        return;
    }
    char const *why = NULL;
    refusal         = NULL;
    if (!mesh_discover_start(&why)) {
        refusal = why ? why : "could not send";
        char text[96];
        snprintf(text, sizeof(text), "Not sent: %s.", refusal);
        lv_label_set_text(status_label, text);
        ESP_LOGW(TAG, "discover nearby refused: %s", refusal);
        return;
    }
    list.sel = 0;
    refresh(NULL);
}

static void ask(void) {
    shell_dialog_t const d = {
        .title         = "Send one request to nearby nodes? It goes on air once, to nodes in direct range.",
        .seconds       = ASK_CONFIRM_S,
        .careful       = true,
        .confirm_label = "send",
    };
    if (!shell_dialog_countdown(&d, ask_answer)) {
        lv_label_set_text(status_label, mc_busy_note());
    }
}

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        mc_list_move(&list, key == LV_KEY_UP ? -1 : 1);
    } else if (key == LV_KEY_ENTER && have_action_row && list.sel == 0) {
        uint32_t wait_s = disc_wait_s(&mem->d, mesh_uptime_s());
        if (wait_s) {
            char text[64];
            snprintf(text, sizeof(text), "Wait %u s before asking again.", (unsigned)wait_s);
            lv_label_set_text(status_label, text);
        } else {
            ask();
        }
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    lichen_free(meshcore_self(), mem);
    mem          = NULL;
    screen_obj   = NULL;
    status_label = NULL;
    list         = (mc_list_t){0};
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    screen_obj       = screen;
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    mem = lichen_alloc(meshcore_self(), sizeof(*mem), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!mem) {
        ESP_LOGE(TAG, "no memory for discover nearby");
        mc_list_label(screen, "Not enough memory for this page.", THEME_DIM);
        return screen;
    }
    list         = (mc_list_t){.keys = mem->keys, .items = mem->items, .cap = DISC_MAX + 1};
    list.list = mc_list_screen(screen, "", "", "Up/Down", "scroll");
    // R1: the kind small and dim over the name, in place of the list screen's one-line heading
    lv_obj_delete(lv_obj_get_child(screen, 0));
    lv_obj_move_to_index(lichen_heading(screen, "MeshCore", "Discover nearby"), 0);
    status_label = lv_obj_get_child(screen, 1); // the note under the heading
    lv_obj_add_event_cb(list.list, on_key, LV_EVENT_KEY, NULL);
    refusal  = NULL;
    sig_seen = 0;
    built_s  = mesh_uptime_s();
    timer    = lv_timer_create(refresh, TICK_MS, NULL);
    refresh(NULL);
    return screen;
}

void meshcore_discover_open(void *arg) {
    (void)arg;
    shell_open(build, SCREEN_MC_DISCOVER);
}
