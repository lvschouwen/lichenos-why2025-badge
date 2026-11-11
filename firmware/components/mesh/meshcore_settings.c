// Settings > MeshCore (#67, settings.html): Name, Region & radio (MeshCore's presets, Custom..., Advanced: TX power),
// Channels (list, add a #hashtag, add with a key) and Send advert, then the about rows (meshcore_app.c).
// The mesh.h setters wait for the mesh lock (MeshCore may be saving contacts for a few hundred ms), so each change runs
// in a short-lived task on core 0, like the Wi-Fi scan; its result comes back to the LVGL task through lichen_ui_call
// and lands on whatever row or note is still on screen. The screens never wait on MeshCore.
#include "lichen_mem.h"
#include "lichen_task.h"
#include "screen_ids.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "menu.h"
#include "menu_filter.h"
#include "lichen_clock.h"
#include "mesh.h"
#include "mesh_chat.h"
#include "mesh_format.h"
#include "mesh_order.h"
#include "lichen_notify.h"
#include "mesh_share.h"
#include "qr_screen.h"
#include "meshcore_run.h"
#include "meshcore_screens.h"
#include "meshcore_settings.h"
#include "mesh_scope.h"
#include "radio_confirm.h"
#include "radio_filter.h"
#include "radio_presets.h"
#include "lichen_ui.h"
#include "text_entry.h"
#include "theme.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char const TAG[] = "meshcore";

#define REMOVE_ASK_S    20 // the careful remove dialog cancels by itself after this
#define JOB_STACK       6144 // savePrefs / saveChannels keep File objects on the stack (the console's 4 KB suffices)
#define MAX_CHANNELS    40   // MAX_GROUP_CHANNELS in the mesh component's build
#define MAX_PRESETS     32
#define KEY_MAX         44   // a 32-byte key in base64
#define TX_SETTLE_MS    1000 // TX power is saved this long after the last step, to spare the flash
#define NAME_CHARS      31
#define UI_CALL_TRIES   100  // lichen_ui_call tries (up to 2.1 s each) before a job's result is dropped

// ---- the change, off the LVGL task -------------------------------------------------------------------------------

typedef enum {
    JOB_NAME,
    JOB_RADIO,
    JOB_PATHHASH,
    JOB_CHANNEL_ADD,
    JOB_CHANNEL_RM,
    JOB_ADVERT,
    JOB_CHANNELS,
    JOB_CHANNEL_INFO,
    JOB_CHANNEL_RENAME,
    JOB_SCOPE,
    JOB_CHANNEL_SCOPE,
} job_kind_t;

typedef struct job {
    job_kind_t  kind;
    char        text[64];
    char        key[KEY_MAX + 1];
    float       freq, bw;
    uint8_t     sf, cr, path_hash_size; // path_hash_size: JOB_PATHHASH only
    int8_t      tx_dbm;
    char        title[40]; // what is being applied, for the result line
    char const *reason;    // result: NULL = done
    bool        ok;
    size_t      n;
    char        names[MAX_CHANNELS][MESH_CHANNEL_MAX + 1];
    mesh_channel_info_t info; // JOB_CHANNEL_INFO (#54): holds a key, wiped with the job
    void (*done)(struct job *);
} job_t;

static bool job_running; // LVGL task only: one change at a time

static void job_finish(void *data) {
    job_t *job  = data;
    job_running = false;
    job->done(job);
    memset(job->key, 0, sizeof(job->key)); // a channel key: not left in the heap
    memset(&job->info, 0, sizeof(job->info));
    lichen_free(meshcore_ctx(), job);
}

static void job_task(void *arg) {
    job_t *job = arg;
    switch (job->kind) {
        case JOB_NAME: job->ok = mesh_set_name(job->text); break;
        case JOB_RADIO:
            job->reason = mesh_set_radio(job->freq, job->bw, job->sf, job->cr, job->tx_dbm);
            break;
        case JOB_PATHHASH: job->reason = mesh_set_path_hash(job->path_hash_size); break;
        case JOB_CHANNEL_ADD: job->reason = mesh_channel_add(job->text, job->key[0] ? job->key : NULL); break;
        case JOB_CHANNEL_RM: job->reason = mesh_channel_remove_named(job->text); break;
        case JOB_ADVERT: job->ok = mesh_send_advert(); break;
        case JOB_CHANNELS: job->n = mesh_channels_snapshot(job->names, MAX_CHANNELS); break;
        case JOB_CHANNEL_INFO: job->n = mesh_channel_info(-1, job->text, &job->info, MESH_SET_WAIT_MS); break;
        case JOB_CHANNEL_RENAME: job->reason = mesh_channel_rename(job->text, job->title); break;
        case JOB_SCOPE: job->reason = mesh_set_scope(job->text); break;
        case JOB_CHANNEL_SCOPE: job->reason = mesh_channel_set_scope(job->text, job->title); break;
    }
    // job_finish frees the job and clears job_running. Each try waits up to 2 s for the LVGL lock: after UI_CALL_TRIES
    // the LVGL task is stuck, so the result is dropped here and the page is not left busy for good
    for (int tries = 0; !lichen_ui_call(job_finish, job); tries++) {
        if (tries + 1 >= UI_CALL_TRIES) {
            ESP_LOGW(TAG, "settings job %d: result not handed to the screen, dropped", (int)job->kind);
            memset(job->key, 0, sizeof(job->key));
            memset(&job->info, 0, sizeof(job->info));
            lichen_free(meshcore_ctx(), job);
            __atomic_store_n(&job_running, false, __ATOMIC_RELEASE);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static job_t *job_new(job_kind_t kind, void (*done)(job_t *)) {
    if (job_running) {
        return NULL;
    }
    job_t *job = lichen_alloc(meshcore_ctx(), sizeof(*job), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (job) {
        job->kind = kind;
        job->done = done;
    }
    return job;
}

// false (job freed) when it could not start
static bool job_start(job_t *job) {
    if (!lichen_task_start(meshcore_ctx(), "meshset", job_task, job, JOB_STACK, LICHEN_PRIO_JOB, NULL)) {
        ESP_LOGW(TAG, "settings task did not start");
        lichen_free(meshcore_ctx(), job);
        return false;
    }
    job_running = true;
    return true;
}

// ---- helpers -----------------------------------------------------------------------------------------------------

#define RETRY_MS 300

static void retry_due(lv_timer_t *timer) {
    void (*fn)(void) = (void (*)(void))lv_timer_get_user_data(timer);
    fn(); // one-shot: LVGL deletes the timer after this call
}

// A refresh that met another change still running: again shortly (fn checks its screen is still there)
static void retry_later(void (*fn)(void)) {
    lv_timer_t *timer = lv_timer_create(retry_due, RETRY_MS, (void *)fn);
    if (timer) {
        lv_timer_set_repeat_count(timer, 1);
    }
}

// The preset in use, as fw mesh names it; -1 for Custom
static int current_preset(mesh_settings_t const *s) {
    return radio_preset_match(s->freq, s->bw, s->sf, s->cr);
}

static void radio_values(char *out, size_t cap, float freq, float bw, uint8_t sf, uint8_t cr, bool marked) {
    char radio[64];
    mesh_format_radio(radio, sizeof(radio), freq, bw, sf, cr);
    snprintf(out, cap, "%s%s", marked ? LV_SYMBOL_OK " " : "", radio);
}

// The value on a preset or custom row: next to a long title only the frequency fits (#131); the rest is in the
// confirmation and on the Custom radio page
static void radio_short(char *out, size_t cap, float freq, bool marked) {
    snprintf(out, cap, "%s%.3f MHz", marked ? LV_SYMBOL_OK " " : "", freq);
}

static void set_note(lv_obj_t *note, char const *text, bool problem) {
    if (note) {
        lv_label_set_text(note, text);
        lv_obj_set_style_text_color(note, lv_color_hex(problem ? THEME_RED : THEME_DIM), 0);
    }
}

// ---- the page ----------------------------------------------------------------------------------------------------

static lv_obj_t *name_row, *radio_row, *channels_row, *advert_row, *scope_row;

static void radio_row_refresh(void) {
    mesh_settings_t s;
    if (radio_row && mesh_get_settings(&s)) {
        int preset = current_preset(&s);
        menu_set_value(radio_row, preset >= 0 ? radio_preset_at((size_t)preset)->title : "Custom");
    }
    meshcore_about_refresh();
}

static void (*first_name_submitted)(void);
static void (*first_name_done)(bool ok); // first-open question in flight (NULL when Name is changed from Settings)
static void (*first_region_chosen)(void);

static void name_done(job_t *job) {
    mesh_settings_t s;
    if (first_name_done) {
        void (*next)(bool) = first_name_done;
        first_name_done    = NULL;
        next(job->ok);
        return;
    }
    if (name_row) {
        menu_set_value(name_row, job->ok && mesh_get_settings(&s) ? s.name : "Not saved: MeshCore is busy");
    }
}

static void edit_name(void *arg);

static void on_name(char const *text, void *arg) {
    (void)arg;
    char clean[NAME_CHARS + 1];
    if (!mesh_name_clean(text, clean, sizeof(clean))) {
        if (first_name_done) {
            void (*next)(bool) = first_name_done;
            first_name_done    = NULL;
            next(false);
        } else {
            menu_set_value(name_row, "Not saved: a name can't be empty");
        }
        return;
    }
    job_t *job = job_new(JOB_NAME, name_done);
    if (!job) {
        if (first_name_done) {
            void (*next)(bool) = first_name_done;
            first_name_done    = NULL;
            next(false);
        } else {
            menu_set_value(name_row, mc_busy_note());
        }
        return;
    }
    snprintf(job->text, sizeof(job->text), "%s", clean);
    if (!first_name_done) {
        menu_set_value(name_row, "Saving...");
    }
    void (*submitted)(void) = first_name_submitted;
    if (!job_start(job)) {
        if (first_name_done) {
            void (*next)(bool) = first_name_done;
            first_name_done    = NULL;
            next(false);
        }
    } else if (first_name_done && submitted) {
        submitted();
    }
}

bool meshcore_ask_name(void (*submitted)(void), void (*done)(bool ok)) {
    lv_obj_t *before = lv_screen_active();
    edit_name(NULL);
    first_name_submitted = submitted;
    first_name_done      = done;
    return lv_screen_active() != before;
}

void meshcore_ask_cancel(void) {
    first_name_submitted = NULL;
    first_name_done      = NULL;
    first_region_chosen  = NULL;
}

static void edit_name(void *arg) {
    (void)arg;
    first_name_done = NULL; // a Name edit from Settings; meshcore_ask_name sets it again after this
    mesh_settings_t s;
    mesh_get_settings(&s);
    text_entry_open("Your name on the mesh", s.name, false, NAME_CHARS, on_name, NULL);
}

// ---- Flood scope (#163) ------------------------------------------------------------------------------------------
// The default region adverts and messages flood in. Setting a region is the careful part (contacts outside it can no
// longer reach you), so it asks first; None (everywhere) is the safe state and resets at once. Sends nothing itself.

static char scope_pending[MESH_SCOPE_MAX + 1]; // the region waiting for the dialog's answer

static void scope_row_refresh(void) {
    mesh_settings_t s;
    char            text[40];
    if (scope_row && mesh_get_settings(&s)) {
        mesh_scope_text(s.scope, text, sizeof(text));
        menu_set_value(scope_row, text);
    }
}

static void scope_done(job_t *job) {
    if (!scope_row) {
        return;
    }
    if (job->reason) {
        menu_set_value(scope_row, job->reason);
    } else {
        scope_row_refresh();
    }
}

static void scope_apply(char const *name) {
    job_t *job = job_new(JOB_SCOPE, scope_done);
    if (!job) {
        menu_set_value(scope_row, mc_busy_note());
        return;
    }
    snprintf(job->text, sizeof(job->text), "%s", name);
    menu_set_value(scope_row, "Saving...");
    if (!job_start(job)) {
        menu_set_value(scope_row, mc_busy_note());
    }
}

static void scope_answer(bool confirmed) {
    if (confirmed && scope_row) {
        scope_apply(scope_pending);
    }
    memset(scope_pending, 0, sizeof(scope_pending));
}

static void on_scope(char const *text, void *arg) {
    (void)arg;
    char        name[MESH_SCOPE_MAX + 1];
    char const *why = mesh_scope_parse(text, name, sizeof(name));
    if (!scope_row) {
        return;
    }
    if (why) {
        char line[80];
        snprintf(line, sizeof(line), "Not saved: %s", why);
        menu_set_value(scope_row, line);
        return;
    }
    mesh_settings_t s;
    if (mesh_get_settings(&s) && strcmp(s.scope, name) == 0) {
        scope_row_refresh(); // nothing changes
        return;
    }
    if (!name[0]) {
        scope_apply(name); // None (everywhere): always, no dialog
        return;
    }
    char                 ask[160];
    shell_dialog_t const d = {
        .title         = ask,
        .seconds       = REMOVE_ASK_S,
        .careful       = true,
        .confirm_label = "set region",
    };
    snprintf(scope_pending, sizeof(scope_pending), "%s", name);
    mesh_scope_ask_text(name, ask, sizeof(ask));
    if (!shell_dialog_countdown(&d, scope_answer)) {
        menu_set_value(scope_row, mc_busy_note());
    }
}

static void edit_scope(void *arg) {
    (void)arg;
    mesh_settings_t s;
    mesh_get_settings(&s);
    text_entry_open("Flood scope: region name, empty = None (everywhere)", s.scope, false, MESH_SCOPE_MAX, on_scope,
                    NULL);
}

static void channels_line(job_t *job, lv_obj_t *row) {
    if (!row) {
        return;
    }
    if (job->n == MESH_BUSY) {
        menu_set_value(row, mc_busy_note());
        return;
    }
    char line[24];
    snprintf(line, sizeof(line), "%u channel%s", (unsigned)job->n, job->n == 1 ? "" : "s");
    menu_set_value(row, job->n ? line : "None");
}

static void page_channels_done(job_t *job) {
    channels_line(job, channels_row);
}

static void page_channels_refresh(void) {
    if (!channels_row) {
        return;
    }
    job_t *job = job_new(JOB_CHANNELS, page_channels_done);
    if (job) {
        job_start(job);
    } else if (job_running) {
        retry_later(page_channels_refresh);
    }
}

// One advert at a time, from the Settings row or the grid's Advertise tile (#88): the result goes to whoever asked
static void (*advert_status)(char const *status);

static void advert_done(job_t *job) {
    char const *note = mesh_advert_note(clock_is_approximate()); // allowed, but say so (#83)
    if (advert_status) {
        advert_status(!job->ok ? "Not sent" : note ? "Queued, approximate clock" : "Queued");
    }
}

void meshcore_advert_send(void (*status)(char const *text)) {
    char const *refusal = mesh_advert_refusal(clock_is_set());
    if (refusal) {
        status(refusal); // no job: nothing is sent
        return;
    }
    job_t *job = job_new(JOB_ADVERT, advert_done);
    if (!job) {
        status(mc_busy_note());
        return;
    }
    advert_status = status;
    status("Sending...");
    if (!job_start(job)) { // the job is freed: no result will come
        advert_status = NULL;
        status(mc_busy_note());
    }
}

void meshcore_advert_forget(void (*status)(char const *text)) {
    if (advert_status == status) {
        advert_status = NULL;
    }
}

static void advert_row_status(char const *text) {
    if (advert_row) {
        menu_set_value(advert_row, text);
    }
}

static void send_advert(void *arg) {
    (void)arg;
    meshcore_advert_send(advert_row_status);
}

static void open_radio(void *arg);
static void open_channels(void *arg);

// ---- running or not (#103): Start at boot, Start / Stop MeshCore -------------------------------------------------

#define RUN_POLL_MS 500

static lv_obj_t   *autostart_row, *run_row; // NULL once the page is gone
static lv_timer_t *run_timer;
static char const *run_refusal; // why the last Enter did nothing, shown until the state changes
static mesh_run_t  run_shown;

static void run_row_refresh(lv_timer_t *timer) {
    (void)timer;
    if (!run_row) {
        return;
    }
    mesh_run_t r = meshcore_run_get();
    if (r.state != run_shown.state || r.why != run_shown.why) {
        run_refusal = NULL;
    }
    run_shown = r;
    menu_set_label(run_row, mesh_run_action_label(r));
    menu_set_value(run_row, run_refusal ? run_refusal : mesh_run_action_value(r));
}

static void toggle_autostart(void *arg) {
    (void)arg;
    bool on = !meshcore_autostart();
    menu_set_value(autostart_row, meshcore_autostart_set(on) ? mesh_run_autostart_text(on) : "Not saved");
}

static void start_stop(void *arg) {
    (void)arg;
    mesh_run_t r = meshcore_run_get();
    if (mesh_run_busy(r) || (r.state == MESH_RUN_STOPPED && r.why == MESH_WHY_UPDATE)) {
        return; // the row already says so
    }
    char const *why = meshcore_run_request(r.state == MESH_RUN_STOPPED);
    run_row_refresh(NULL);
    if (why) {
        run_refusal = why;
        menu_set_value(run_row, why);
    }
}

static void run_rows(lv_obj_t *page) {
    char const *on = mesh_run_autostart_text(meshcore_autostart());
    autostart_row  = menu_add(page, "Start at boot", on, toggle_autostart, NULL);
    menu_set_kind(autostart_row, MENU_TOGGLE);
    run_row       = menu_add(page, "", "", start_stop, NULL);
    run_refusal   = NULL;
    run_shown     = meshcore_run_get();
    run_row_refresh(NULL);
    run_timer = lv_timer_create(run_row_refresh, RUN_POLL_MS, NULL); // a start or stop ends on the job task
}

// ---- the grid's tile order (#189) --------------------------------------------------------------------------------

static lv_obj_t *order_row; // "Reset tile order": its value says Default or Arranged

static void order_row_refresh(void) {
    if (order_row) {
        menu_set_value(order_row, mesh_order_arranged() ? "Arranged" : "Default");
    }
}

static void arrange_tiles(void *arg) {
    (void)arg;
    if (!meshcore_grid_arrange_start()) {
        ESP_LOGW(TAG, "arrange tiles: the grid is not under this page");
    }
}

static void reset_order_answer(bool yes) {
    if (yes) {
        if (!mesh_order_reset()) {
            ESP_LOGE(TAG, "tile order not reset");
            lichen_notify("meshcore", LN_WARN, "Tile order not reset",
                          "The tiles kept their places. Try again; if it keeps failing, restart the badge.", 0);
            return;
        }
        meshcore_grid_reload(); // a grid under this page shows the first order at once
        order_row_refresh();
    }
}

static void ask_reset_order(void *arg) {
    (void)arg;
    shell_dialog_t const d = {
        .title         = "Reset the tile order? The tiles go back to their first places.",
        .seconds       = 10,
        .careful       = true, // an arrangement takes work to redo: running out of time cancels
        .confirm_label = "reset",
    };
    shell_dialog_countdown(&d, reset_order_answer);
}

// "Arrange tiles" only where the grid is under this page (Settings tile): from Lichen's Settings the app is not open
static void order_rows(lv_obj_t *page) {
    if (meshcore_grid_arrange_possible()) {
        menu_add(page, "Arrange tiles", "", arrange_tiles, NULL); // an action, not a page: it returns to the grid
    }
    order_row = menu_add(page, "Reset tile order", "", ask_reset_order, NULL);
    order_row_refresh();
}

static void on_page_delete(lv_event_t *e) {
    (void)e;
    name_row = radio_row = channels_row = advert_row = scope_row = NULL;
    autostart_row = run_row = order_row = NULL;
    if (run_timer) {
        lv_timer_delete(run_timer);
        run_timer = NULL;
    }
    meshcore_advert_forget(advert_row_status);
}

static void open_notify_page(void *arg) {
    (void)arg;
    meshcore_notify_settings_open(); // #50: the same page as Settings > Notifications > MeshCore
}

void meshcore_settings(lv_obj_t *page) {
    run_rows(page);
    lv_obj_add_event_cb(page, on_page_delete, LV_EVENT_DELETE, NULL);
    mesh_settings_t s;
    if (!mesh_running() || !mesh_get_settings(&s)) {
        menu_note(page, "MeshCore is not running: its settings come back when it does.");
        menu_set_kind(menu_add(page, "Notifications", "", open_notify_page, NULL), MENU_OPEN);
        order_rows(page);
        return;
    }
    name_row     = menu_add(page, "Name", s.name, edit_name, NULL);
    radio_row    = menu_add(page, "Region & radio", "", open_radio, NULL);
    scope_row    = menu_add(page, "Flood scope", "", edit_scope, NULL);
    scope_row_refresh();
    channels_row = menu_add(page, "Channels", "", open_channels, NULL);
    advert_row   = menu_add(page, "Send advert", "", send_advert, NULL);
    menu_set_kind(menu_add(page, "Notifications", "", open_notify_page, NULL), MENU_OPEN);
    menu_set_kind(menu_add(page, "Advert & contacts", "", meshcore_advert_settings_open, NULL), MENU_OPEN);
    menu_set_kind(menu_add(page, "Radio health", "", meshcore_health_open, NULL), MENU_OPEN);
    menu_set_kind(menu_add(page, "Air log", "", meshcore_airlog_open, NULL), MENU_OPEN);
    order_rows(page);
    menu_set_kind(radio_row, MENU_OPEN);
    menu_set_kind(channels_row, MENU_OPEN);
    radio_row_refresh();
    page_channels_refresh();
}

// ---- Region & radio ----------------------------------------------------------------------------------------------

static lv_obj_t   *preset_rows[MAX_PRESETS];
static lv_obj_t   *custom_row, *tx_note, *phs_row;
static lv_timer_t *tx_timer;
static int8_t      tx_wanted;

static void marks_refresh(void) {
    mesh_settings_t s;
    if (!custom_row || !mesh_get_settings(&s)) {
        return;
    }
    int  cur = current_preset(&s);
    char text[80];
    for (size_t i = 0; i < radio_preset_count() && i < MAX_PRESETS; i++) {
        radio_preset_t const *p = radio_preset_at(i);
        radio_short(text, sizeof(text), p->freq_mhz, (int)i == cur);
        menu_set_value(preset_rows[i], text);
    }
    if (cur < 0) {
        radio_short(text, sizeof(text), s.freq, true);
        menu_set_value(custom_row, text);
    } else {
        menu_set_value(custom_row, "");
    }
}

static void region_chosen(void) {
    void (*next)(void) = first_region_chosen;
    first_region_chosen = NULL;
    if (next) {
        next();
    }
}

static void preset_done(job_t *job) {
    if (job->reason) {
        if (custom_row && job->n < MAX_PRESETS) {
            menu_set_value(preset_rows[job->n], job->reason); // in place: the row that was chosen
        }
        return;
    }
    marks_refresh();
    radio_row_refresh();
    region_chosen();
}

// Switching the radio asks first (#33): a stray Enter put the badge on a frequency that may not be legal here. The
// confirm page shows the new radio; Enter runs `go` on the screen below it, Esc keeps the current radio.
static struct {
    char            title[40];
    float           freq, bw;
    uint8_t         sf, cr;
    radio_confirm_t level;
    void (*go)(void *);
    void *arg;
} want;

static void switch_yes(void *arg) {
    (void)arg;
    shell_back_then(want.go, want.arg);
}

static lv_obj_t *build_switch(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Switch radio");
    char      text[96];
    lv_obj_t *what = menu_note(menu, want.title);
    lv_obj_set_style_text_color(what, lv_color_hex(THEME_TEXT), 0);
    radio_values(text, sizeof(text), want.freq, want.bw, want.sf, want.cr, false);
    menu_note(menu, text);
    menu_note(menu, "The badge is on this radio from the next message on.");
    if (want.level == RADIO_CONFIRM_BAND) {
        set_note(menu_note(menu, ""), "Another band than the current radio. It may not be legal where you are.", true);
    }
    menu_add(menu, "Switch radio", "", switch_yes, NULL);
    shell_page_hint(screen, "Enter", "switch radio", "Esc", "cancel", NULL);
    return screen;
}

// Asks, or applies at once when the first-run question is being answered (that list is the question itself)
static void confirm_switch(char const *title, float freq, float bw, uint8_t sf, uint8_t cr, radio_confirm_t level,
                           void (*go)(void *), void *arg) {
    if (first_region_chosen) {
        go(arg);
        return;
    }
    snprintf(want.title, sizeof(want.title), "%s", title);
    want.freq  = freq;
    want.bw    = bw;
    want.sf    = sf;
    want.cr    = cr;
    want.level = level;
    want.go    = go;
    want.arg   = arg;
    shell_open(build_switch, SCREEN_SWITCH);
}

static void preset_go(void *arg) {
    size_t                i = (size_t)(uintptr_t)arg;
    radio_preset_t const *p = radio_preset_at(i);
    mesh_settings_t       s;
    if (!p || !mesh_get_settings(&s)) {
        return;
    }
    job_t *job = job_new(JOB_RADIO, preset_done);
    if (!job) {
        menu_set_value(preset_rows[i], mc_busy_note());
        return;
    }
    job->freq           = p->freq_mhz;
    job->bw             = p->bw_khz;
    job->sf             = p->sf;
    job->cr             = p->cr;
    job->tx_dbm         = s.tx_dbm;
    job->n              = i; // the row for the result
    menu_set_value(preset_rows[i], "Applying...");
    job_start(job);
}

static void pick_preset(void *arg) {
    size_t                i = (size_t)(uintptr_t)arg;
    radio_preset_t const *p = radio_preset_at(i);
    mesh_settings_t       s;
    if (!p || !mesh_get_settings(&s)) {
        return;
    }
    radio_confirm_t level = radio_confirm_needed(s.freq, s.bw, s.sf, s.cr, 0, p->freq_mhz, p->bw_khz,
                                                 p->sf, p->cr, 0);
    if (level == RADIO_CONFIRM_NONE) {
        region_chosen(); // already in use: nothing to switch (the first-run question counts it as the answer)
        return;
    }
    confirm_switch(p->title, p->freq_mhz, p->bw_khz, p->sf, p->cr, level, preset_go, arg);
}

// A TX power change is never lost quietly (review R1): it waits for a change still running, is tried TX_TRIES times
// when MeshCore refuses (busy), and goes on after its screen is gone; only the last failure is given up, with a warning.
#define TX_TRIES 3
static int tx_failures;

static void tx_apply(void);

static void tx_done(job_t *job) {
    char text[80];
    if (!job->reason) {
        tx_failures = 0;
        snprintf(text, sizeof(text), "TX power %d dBm saved", job->tx_dbm);
        set_note(tx_note, text, false);
        meshcore_about_refresh();
        return;
    }
    if (++tx_failures < TX_TRIES) {
        snprintf(text, sizeof(text), "TX power not saved yet (%s): trying again", job->reason);
        set_note(tx_note, text, true);
        retry_later(tx_apply);
        return;
    }
    tx_failures = 0;
    ESP_LOGW(TAG, "TX power %d dBm not saved: %s", job->tx_dbm, job->reason);
    snprintf(text, sizeof(text), "TX power not changed: %s", job->reason);
    set_note(tx_note, text, true);
}

// Needs no screen: it also runs after Region & radio was left
static void tx_apply(void) {
    mesh_settings_t s;
    if (!mesh_get_settings(&s) || s.tx_dbm == tx_wanted) {
        return;
    }
    job_t *job = job_new(JOB_RADIO, tx_done);
    if (!job) {
        if (job_running) {
            retry_later(tx_apply); // another change first
        } else {
            ESP_LOGW(TAG, "TX power %d dBm not saved: no memory", tx_wanted);
            set_note(tx_note, "TX power not changed: no memory", true);
        }
        return;
    }
    job->freq           = s.freq;
    job->bw             = s.bw;
    job->sf             = s.sf;
    job->cr             = s.cr;
    job->tx_dbm         = tx_wanted;
    job_start(job);
}

static void tx_settled(lv_timer_t *timer) {
    (void)timer;
    tx_timer = NULL; // one-shot: LVGL deletes it after this call
    tx_apply();
}

static void on_tx(int value, void *arg) {
    (void)arg;
    tx_wanted = (int8_t)value;
    if (tx_timer) {
        lv_timer_reset(tx_timer);
    } else {
        tx_timer = lv_timer_create(tx_settled, TX_SETTLE_MS, NULL);
        lv_timer_set_repeat_count(tx_timer, 1);
    }
}

static void open_custom(void *arg);

// Path hash size (#129): its own setting, not part of a preset. Changing it sends nothing, so no confirm page.
static void phs_show(uint8_t size) {
    char text[16];
    snprintf(text, sizeof(text), size == 1 ? "1 byte" : "%u bytes", size);
    if (phs_row) {
        menu_set_value(phs_row, text);
    }
}

static void phs_done(job_t *job) {
    mesh_settings_t s;
    if (!job->reason && mesh_get_settings(&s)) {
        phs_show(s.path_hash_size);
    } else if (job->reason && phs_row) {
        menu_set_value(phs_row, job->reason);
    }
}

// Enter: 1 byte <-> 2 bytes (a 3 byte size, set from the console, goes back to 1)
static void toggle_phs(void *arg) {
    (void)arg;
    mesh_settings_t s;
    if (!mesh_get_settings(&s)) {
        return;
    }
    job_t *job = job_new(JOB_PATHHASH, phs_done);
    if (!job) {
        menu_set_value(phs_row, mc_busy_note());
        return;
    }
    job->path_hash_size = s.path_hash_size == 1 ? 2 : 1;
    job_start(job);
}

static void on_radio_delete(lv_event_t *e) {
    (void)e;
    memset(preset_rows, 0, sizeof(preset_rows));
    custom_row = NULL;
    tx_note    = NULL;
    phs_row    = NULL;
    first_region_chosen = NULL; // Esc: the question is not answered
    if (tx_timer) { // left before the power settled: saved now, without the screen (tx_note is NULL)
        lv_timer_delete(tx_timer);
        tx_timer = NULL;
        tx_apply();
    }
}

static bool radio_row_matches(size_t preset, char const *query, void *ctx) {
    (void)ctx;
    return radio_filter_match(preset, query);
}

static lv_obj_t *build_radio(void) {
    mesh_settings_t s;
    if (!mesh_get_settings(&s)) {
        return NULL;
    }
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Region & radio");
    menu_filter_t *filter = menu_filter_create(screen, menu, radio_row_matches, NULL); // #32
    meshcore_applied_notes(menu); // what the C6 runs, and whether it is what MeshCore asks for (#335)
    menu_note(menu, "Presets");
    for (size_t i = 0; i < radio_preset_count() && i < MAX_PRESETS; i++) {
        preset_rows[i] = menu_add(menu, radio_preset_at(i)->title, "", pick_preset, (void *)(uintptr_t)i);
        menu_filter_add(filter, preset_rows[i], i);
    }
    custom_row = menu_add(menu, "Custom...", "", open_custom, NULL);
    menu_filter_fallback(filter, custom_row);
    menu_note(menu, "Advanced");
    phs_row = menu_add(menu, "Path hash size", "", toggle_phs, NULL);
    menu_set_kind(phs_row, MENU_CYCLE);
    phs_show(s.path_hash_size);
    tx_wanted = s.tx_dbm;
    menu_add_slider(menu, "TX power (dBm)", -9, 22, s.tx_dbm, on_tx, NULL);
    tx_note = menu_note(menu, "Left / Right: TX power, saved after a second");
    lv_obj_add_event_cb(screen, on_radio_delete, LV_EVENT_DELETE, NULL);
    marks_refresh();
    int cur = current_preset(&s);
    menu_focus_row(menu, cur >= 0 ? cur : (int)radio_preset_count());
    return screen;
}

static void open_radio(void *arg) {
    (void)arg;
    shell_open(build_radio, SCREEN_RADIO);
}

bool meshcore_ask_region(void (*chosen)(void)) {
    lv_obj_t *before    = lv_screen_active();
    first_region_chosen = chosen;
    shell_open(build_radio, SCREEN_RADIO);
    return lv_screen_active() != before;
}

// ---- Custom... ---------------------------------------------------------------------------------------------------


static float      draft_freq;
static size_t     draft_bw;
static uint8_t    draft_sf, draft_cr;
static lv_obj_t  *freq_row, *bw_row, *sf_row, *cr_row, *custom_note;

static void draft_show(void) {
    char text[32];
    if (isnan(draft_freq)) {
        snprintf(text, sizeof(text), "?");
    } else {
        snprintf(text, sizeof(text), "%.3f MHz", draft_freq);
    }
    menu_set_value(freq_row, text);
    snprintf(text, sizeof(text), "%g kHz", radio_bandwidth_at(draft_bw));
    menu_set_value(bw_row, text);
    snprintf(text, sizeof(text), "SF%u", draft_sf);
    menu_set_value(sf_row, text);
    snprintf(text, sizeof(text), "CR%u", draft_cr);
    menu_set_value(cr_row, text);
}

static void on_freq(char const *text, void *arg) {
    (void)arg;
    char *end;
    float v = strtof(text, &end);
    while (*end == ' ') {
        end++;
    }
    if (end == text || *end) {
        set_note(custom_note, "Not a number: type the frequency in MHz, like 869.618", true);
        return;
    }
    draft_freq = v; // checked on Apply, so an out-of-range value gets its reason there
    draft_show();
    set_note(custom_note, "Enter on Apply to use these values", false);
}

static void edit_freq(void *arg) {
    (void)arg;
    char now[16];
    snprintf(now, sizeof(now), "%.3f", draft_freq);
    text_entry_open("Frequency in MHz", now, false, 12, on_freq, NULL);
}

static void next_bw(void *arg) {
    (void)arg;
    draft_bw = (draft_bw + 1) % radio_bandwidth_count();
    draft_show();
}

static void next_sf(void *arg) {
    (void)arg;
    draft_sf = draft_sf >= 12 ? 5 : draft_sf + 1;
    draft_show();
}

static void next_cr(void *arg) {
    (void)arg;
    draft_cr = draft_cr >= 8 ? 5 : draft_cr + 1;
    draft_show();
}

static void custom_done(job_t *job) {
    if (job->reason) {
        set_note(custom_note, job->reason, true);
        return;
    }
    set_note(custom_note, "Applied and saved", false);
    marks_refresh();
    radio_row_refresh();
    region_chosen();
}

// Runs on the Custom radio screen, after the confirm page is popped
static void custom_go(void *arg) {
    (void)arg;
    mesh_settings_t s;
    if (!mesh_get_settings(&s)) {
        return;
    }
    job_t *job = job_new(JOB_RADIO, custom_done);
    if (!job) {
        set_note(custom_note, mc_busy_note(), true);
        return;
    }
    job->freq           = draft_freq;
    job->bw             = radio_bandwidth_at(draft_bw);
    job->sf             = draft_sf;
    job->cr             = draft_cr;
    job->tx_dbm         = s.tx_dbm;
    set_note(custom_note, "Applying...", false);
    job_start(job);
}

static void apply_custom(void *arg) {
    (void)arg;
    mesh_settings_t s;
    if (!mesh_get_settings(&s)) {
        return;
    }
    // Refused here too, before any task: the reason shows at once
    char const *why = radio_check(draft_freq, radio_bandwidth_at(draft_bw), draft_sf, draft_cr, s.tx_dbm);
    if (why) {
        set_note(custom_note, why, true);
        return;
    }
    radio_confirm_t level = radio_confirm_needed(s.freq, s.bw, s.sf, s.cr, 0, draft_freq,
                                                 radio_bandwidth_at(draft_bw), draft_sf, draft_cr, 0);
    if (level == RADIO_CONFIRM_NONE) {
        set_note(custom_note, "Already using these values", false);
        region_chosen();
        return;
    }
    confirm_switch("Custom radio", draft_freq, radio_bandwidth_at(draft_bw), draft_sf, draft_cr, level, custom_go, NULL);
}

static void on_custom_delete(lv_event_t *e) {
    (void)e;
    freq_row = bw_row = sf_row = cr_row = custom_note = NULL;
}

static lv_obj_t *build_custom(void) {
    mesh_settings_t s;
    if (!mesh_get_settings(&s)) {
        return NULL;
    }
    draft_freq = s.freq;
    draft_sf   = s.sf;
    draft_cr   = s.cr;
    draft_bw   = 6; // 62.5
    for (size_t i = 0; i < radio_bandwidth_count(); i++) {
        if (fabsf(radio_bandwidth_at(i) - s.bw) < 0.01f) {
            draft_bw = i;
        }
    }
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Custom radio");
    menu_note(menu, "Enter changes a value. Nothing is used until Apply.");
    meshcore_applied_notes(menu); // #335
    freq_row    = menu_add(menu, "Frequency", "", edit_freq, NULL);
    bw_row      = menu_add(menu, "Bandwidth", "", next_bw, NULL);
    sf_row      = menu_add(menu, "Spreading factor", "", next_sf, NULL);
    cr_row      = menu_add(menu, "Coding rate", "", next_cr, NULL);
    menu_set_kind(bw_row, MENU_CYCLE);
    menu_set_kind(sf_row, MENU_CYCLE);
    menu_set_kind(cr_row, MENU_CYCLE);
    menu_add(menu, "Apply", "", apply_custom, NULL);
    custom_note = menu_note(menu, "862 to 928 MHz on this badge");
    lv_obj_add_event_cb(screen, on_custom_delete, LV_EVENT_DELETE, NULL);
    draft_show();
    return screen;
}

static void open_custom(void *arg) {
    (void)arg;
    shell_open(build_custom, SCREEN_CUSTOM);
}

// ---- Channels ----------------------------------------------------------------------------------------------------

static lv_obj_t *channel_rows[MAX_CHANNELS + 1]; // the channels, or one placeholder
static size_t    channel_row_count;
static lv_obj_t *add_tag_row, *channel_note;
static char      pending_name[MESH_CHANNEL_MAX + 1];
EXT_RAM_BSS_ATTR static char row_names[MAX_CHANNELS][MESH_CHANNEL_MAX + 1]; // what Enter on a channel row opens
static lv_obj_t *channels_screen;                               // the list, while it exists
static lv_obj_t *channel_screen;                                // a channel's details (#54), while they exist
static void      remove_ask(void *arg);
static void      open_channel(void *arg);

typedef struct {
    job_t    *job;
    lv_obj_t *menu;
} rows_build_t;

// With the screen's group as the default (lichen_ui_with_group), so the new rows join it
static void channel_rows_build(void *arg) {
    rows_build_t const *b    = arg;
    job_t              *job  = b->job;
    lv_obj_t           *menu = b->menu;
    for (size_t i = 0; i < channel_row_count; i++) {
        lv_obj_delete(channel_rows[i]);
    }
    channel_row_count = 0;
    size_t n          = job->n == MESH_BUSY ? 0 : job->n;
    for (size_t i = 0; i < n; i++) {
        snprintf(row_names[i], sizeof(row_names[i]), "%s", job->names[i]);
        // Enter opens the channel's details: key, link, Rename, Remove (#54)
        channel_rows[channel_row_count++] = menu_add(menu, job->names[i], "", open_channel, row_names[i]);
    }
    if (job->n == MESH_BUSY) {
        channel_rows[channel_row_count++] = menu_add(menu, mc_busy_note(), "", NULL, NULL);
    } else if (!n) {
        menu_empty(menu, "channels", "add a #hashtag channel below"); // R11: not a row, gone when the first one comes
    }
}

static void list_done(job_t *job) {
    channels_line(job, channels_row);
    if (!add_tag_row) {
        return; // the screen is gone
    }
    lv_group_t *group = lv_obj_get_group(add_tag_row);
    // Where the selection was, so a removal leaves it on the next channel instead of jumping to the last row
    size_t keep = 0;
    for (size_t i = 0; i < channel_row_count; i++) {
        if (channel_rows[i] == lv_group_get_focused(group)) {
            keep = i;
        }
    }
    rows_build_t build = {.job = job, .menu = lv_obj_get_parent(add_tag_row)};
    lichen_ui_with_group(group, channel_rows_build, &build);
    // After the heading, before the add rows
    for (size_t i = 0; i < channel_row_count; i++) {
        if (channel_rows[i]) {
            lv_obj_move_to_index(channel_rows[i], (int32_t)(1 + i));
        }
    }
    lv_obj_t *focus = channel_row_count ? channel_rows[keep < channel_row_count ? keep : channel_row_count - 1] : NULL;
    if (focus) {
        lv_group_focus_obj(focus);
    }
}

static void list_refresh(void) {
    if (!add_tag_row) {
        return;
    }
    job_t *job = job_new(JOB_CHANNELS, list_done);
    if (job) {
        job_start(job);
    } else if (job_running) {
        retry_later(list_refresh);
    }
}

static void add_done(job_t *job) {
    char text[96];
    if (job->reason) {
        snprintf(text, sizeof(text), "%s not added: %s", job->text, job->reason);
    } else {
        snprintf(text, sizeof(text), "%s added", job->text);
    }
    set_note(channel_note, text, job->reason != NULL);
    if (!job->reason) {
        if (add_tag_row) {
            list_refresh(); // also refreshes the page's Channels line
        } else {
            page_channels_refresh();
        }
    }
}

static void add_channel(char const *name, char const *key) {
    job_t *job = job_new(JOB_CHANNEL_ADD, add_done);
    if (!job) {
        set_note(channel_note, mc_busy_note(), true);
        return;
    }
    snprintf(job->text, sizeof(job->text), "%s", name);
    if (key) {
        snprintf(job->key, sizeof(job->key), "%s", key);
    }
    set_note(channel_note, "Adding...", false);
    job_start(job);
}

// Enter on a channel row: "Remove <name>?" (Enter removes, Esc cancels), the result in the list's note
static char remove_name[MESH_CHANNEL_MAX + 1];

static void remove_done(job_t *job) {
    char text[96];
    if (job->reason) {
        snprintf(text, sizeof(text), "%s not removed: %s", job->text, job->reason);
    } else {
        snprintf(text, sizeof(text), "%s removed", job->text);
    }
    set_note(channel_note, text, job->reason != NULL);
    if (!job->reason) {
        if (channel_screen && channels_screen) {
            shell_back_to(channels_screen, NULL, NULL); // the details of a channel that is gone
        }
        if (add_tag_row) {
            list_refresh();
        } else {
            page_channels_refresh();
        }
    }
}

// Runs on the channel list (or its details), after the confirm dialog
static void remove_go(void) {
    job_t *job = job_new(JOB_CHANNEL_RM, remove_done);
    if (!job) {
        set_note(channel_note, mc_busy_note(), true);
        return;
    }
    snprintf(job->text, sizeof(job->text), "%s", remove_name);
    set_note(channel_note, "Removing...", false);
    job_start(job);
}

static void remove_answer(bool yes) {
    if (yes) {
        remove_go();
    }
}

// A careful dialog: a held Enter (LVGL repeats it) never confirms, only a fresh press after a second does (#49 review)
static void remove_ask(void *arg) {
    snprintf(remove_name, sizeof(remove_name), "%s", (char const *)arg);
    char                 title[MESH_CHANNEL_MAX + 64];
    shell_dialog_t const d = {
        .title        = title,
        .seconds      = REMOVE_ASK_S,
        .careful      = true,
        .confirm_label = "remove",
    };
    snprintf(title, sizeof(title), "Remove %s? Messages already received stay.", remove_name);
    if (!shell_dialog_countdown(&d, remove_answer)) {
        set_note(channel_note, mc_busy_note(), true);
    }
}

static void on_tag(char const *text, void *arg) {
    (void)arg;
    add_channel(text, NULL);
}

static void add_tag(void *arg) {
    (void)arg;
    text_entry_open("Hashtag channel", "#", false, MESH_CHANNEL_MAX, on_tag, NULL);
}

static void on_key_text(char const *text, void *arg) {
    (void)arg;
    add_channel(pending_name, text);
    memset(pending_name, 0, sizeof(pending_name));
}

static void on_key_name(char const *text, void *arg) {
    (void)arg;
    if (!mesh_name_clean(text, pending_name, sizeof(pending_name))) {
        set_note(channel_note, "The channel needs a name", true);
        return;
    }
    // Opened from the first entry's done: that screen is gone by now
    text_entry_open("Channel key (base64)", "", true, KEY_MAX, on_key_text, NULL);
}

static void add_keyed(void *arg) {
    (void)arg;
    text_entry_open("Channel name", "", false, MESH_CHANNEL_MAX, on_key_name, NULL);
}

static void on_channels_delete(lv_event_t *e) {
    (void)e;
    memset(channel_rows, 0, sizeof(channel_rows));
    channel_row_count = 0;
    add_tag_row       = NULL;
    channel_note      = NULL;
    channels_screen   = NULL;
}

// ---- one channel's details (#54): flood scope (#291), key as hex and base64, app link, Rename, Remove ------

typedef struct {
    char name[MESH_CHANNEL_MAX + 1];
    char hex[MESH_KEY_HEX_MAX], b64[MESH_KEY_B64_MAX], uri[MESH_CHANNEL_URI_MAX];
    char scope[MESH_SCOPE_MAX + 1]; // its own flood scope (#291), "" = the default
} channel_mem_t;

static channel_mem_t *chan_mem; // PSRAM, while the details exist; holds a key, wiped when they go
static lv_obj_t      *chan_menu, *chan_loading, *chan_note, *chan_scope_row;
static lv_group_t    *chan_group; // the details screen's keys

static lv_obj_t *add_value(lv_obj_t *menu, char const *text) {
    lv_obj_t *label = menu_note(menu, text);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_TEXT), 0);
    return label;
}

static void rename_done(job_t *job) {
    char text[160];
    if (job->reason) {
        snprintf(text, sizeof(text), "%s not renamed: %s", job->text, job->reason);
        set_note(chan_note, text, true);
        return;
    }
    snprintf(text, sizeof(text), "%s is now %s", job->text, job->title);
    set_note(channel_note, text, false);
    if (channel_screen && channels_screen) {
        shell_back_to(channels_screen, NULL, NULL);
    }
    if (add_tag_row) {
        list_refresh();
    }
}

static void on_rename(char const *text, void *arg) {
    (void)arg;
    job_t *job = chan_mem ? job_new(JOB_CHANNEL_RENAME, rename_done) : NULL;
    if (!job) {
        set_note(chan_note, mc_busy_note(), true);
        return;
    }
    snprintf(job->text, sizeof(job->text), "%s", chan_mem->name);
    snprintf(job->title, sizeof(job->title), "%s", text);
    set_note(chan_note, "Renaming...", false);
    job_start(job);
}

static void ask_rename(void *arg) {
    (void)arg;
    if (chan_mem) {
        text_entry_open("New channel name", chan_mem->name, false, MESH_CHANNEL_MAX, on_rename, NULL);
    }
}

static void show_channel_qr(void *arg) {
    (void)arg;
    if (chan_mem) {
        qr_screen_open("Channel QR code", chan_mem->name, chan_mem->uri, NULL);
    }
}

// ---- a channel's own flood scope (#291) -----------------------------------------------------------------------------
// As the default's row (#163): the value is the state (R3), a region asks first in a careful dialog (R3: contacts
// outside it no longer get the messages), back to the default is the safe state and needs none. Sends nothing itself.

static char chan_scope_pending[MESH_SCOPE_MAX + 1]; // the region waiting for the dialog's answer

static void chan_scope_show(void) {
    mesh_settings_t s;
    char            text[48];
    if (chan_scope_row && chan_mem) {
        mesh_get_settings(&s); // zeroed (None) before MeshCore is up
        mesh_scope_channel_text(chan_mem->scope, s.scope, text, sizeof(text));
        menu_set_value(chan_scope_row, text);
    }
}

#define CHAN_SCOPE_NOTE_MS 3000 // a "Not saved" stays this long, then the row shows the state again (R3)

static lv_timer_t *chan_scope_timer; // the pending one-shot back to the state, NULL when none

static void chan_scope_show_due(lv_timer_t *timer) {
    (void)timer;
    chan_scope_timer = NULL; // one-shot: LVGL deletes it after this call
    chan_scope_show();       // a no-op when the details are gone
}

// R5 and R3: why it was not saved, then the real state again (the value never stays a stale message)
static void chan_scope_not_saved(char const *why) {
    char line[80];
    snprintf(line, sizeof(line), "Not saved: %s", why);
    menu_set_value(chan_scope_row, line);
    if (chan_scope_timer) {
        lv_timer_delete(chan_scope_timer); // a newer note: its own full time
    }
    chan_scope_timer = lv_timer_create(chan_scope_show_due, CHAN_SCOPE_NOTE_MS, NULL);
    if (chan_scope_timer) {
        lv_timer_set_repeat_count(chan_scope_timer, 1);
    }
}

static void chan_scope_done(job_t *job) {
    if (!chan_scope_row || !chan_mem || strcmp(chan_mem->name, job->text) != 0) {
        return; // the details are gone, or are another channel's now
    }
    if (job->reason) {
        chan_scope_not_saved(job->reason);
        return;
    }
    snprintf(chan_mem->scope, sizeof(chan_mem->scope), "%.30s", job->title); // a region: 30 at most
    chan_scope_show();
}

static void chan_scope_apply(char const *region) {
    job_t *job = chan_mem ? job_new(JOB_CHANNEL_SCOPE, chan_scope_done) : NULL;
    if (!job) {
        menu_set_value(chan_scope_row, mc_busy_note());
        return;
    }
    snprintf(job->text, sizeof(job->text), "%s", chan_mem->name);
    snprintf(job->title, sizeof(job->title), "%s", region);
    menu_set_value(chan_scope_row, "Saving..."); // R10: at once; R5: the result replaces it
    if (!job_start(job)) {
        menu_set_value(chan_scope_row, mc_busy_note());
    }
}

static void chan_scope_answer(bool confirmed) {
    if (confirmed && chan_scope_row) {
        chan_scope_apply(chan_scope_pending);
    }
    memset(chan_scope_pending, 0, sizeof(chan_scope_pending));
}

static void on_chan_scope(char const *text, void *arg) {
    (void)arg;
    char        name[MESH_SCOPE_MAX + 1];
    char const *why = mesh_scope_parse(text, name, sizeof(name));
    if (!chan_scope_row || !chan_mem) {
        return;
    }
    if (why) {
        chan_scope_not_saved(why);
        return;
    }
    if (strcmp(chan_mem->scope, name) == 0) {
        chan_scope_show(); // nothing changes
        return;
    }
    if (!name[0]) {
        chan_scope_apply(name); // back to the default: always, no dialog
        return;
    }
    char                 ask[200];
    shell_dialog_t const d = {
        .title         = ask,
        .seconds       = REMOVE_ASK_S,
        .careful       = true,
        .confirm_label = "set region",
    };
    snprintf(chan_scope_pending, sizeof(chan_scope_pending), "%s", name);
    mesh_scope_channel_ask_text(chan_mem->name, name, ask, sizeof(ask));
    if (!shell_dialog_countdown(&d, chan_scope_answer)) {
        menu_set_value(chan_scope_row, mc_busy_note());
    }
}

static void edit_chan_scope(void *arg) {
    (void)arg;
    if (chan_mem) {
        bool damaged = strcmp(chan_mem->scope, MESH_CHSCOPE_DAMAGED) == 0; // starts empty then, never with "!"
        text_entry_open("Flood scope for this channel: region name, empty = default", damaged ? "" : chan_mem->scope,
                        false, MESH_SCOPE_MAX, on_chan_scope, NULL);
    }
}

static void channel_actions_build(void *arg) {
    mesh_channel_info_t const *info = arg;
    chan_scope_row                  = menu_add(chan_menu, "Flood scope", "", edit_chan_scope, NULL); // R9: as Settings
    chan_scope_show();
    menu_add(chan_menu, "Rename", "", ask_rename, NULL);
    if (chan_mem->uri[0]) { // an app link exists only for 16-byte keys
        menu_set_kind(menu_add(chan_menu, "Show as QR", "", show_channel_qr, NULL), MENU_OPEN);
    }
    if (info->index != 0) {
        menu_add(chan_menu, "Remove", "", remove_ask, chan_mem->name); // Public (slot 0) stays
    }
}

static void info_done(job_t *job) {
    if (!chan_menu || !chan_mem) {
        return; // the details are gone
    }
    lv_obj_delete(chan_loading);
    chan_loading = NULL;
    if (job->n != 1) {
        menu_note(chan_menu, job->n == 0 ? "This channel is gone." : mc_busy_note());
        return;
    }
    mesh_channel_info_t const *info = &job->info;
    mesh_hex_text(info->secret, info->key_len, chan_mem->hex, sizeof(chan_mem->hex));
    mesh_base64_text(info->secret, info->key_len, chan_mem->b64, sizeof(chan_mem->b64));
    bool link = mesh_channel_uri(info->name, info->secret, info->key_len, chan_mem->uri, sizeof(chan_mem->uri));
    snprintf(chan_mem->scope, sizeof(chan_mem->scope), "%s", info->scope);
    lichen_ui_with_group(chan_group, channel_actions_build, (void *)info); // the new rows join the screen's group
    menu_focus_row(chan_menu, 0);
    menu_note(chan_menu, info->key_len == 16 ? "Key (16 bytes, hex):" : "Key (32 bytes, hex):");
    add_value(chan_menu, chan_mem->hex);
    menu_note(chan_menu, "Key (base64, as Add channel with key takes it):");
    add_value(chan_menu, chan_mem->b64);
    menu_note(chan_menu, link ? "Link for MeshCore apps (Add channel, paste):"
                              : "No app link: MeshCore's apps only take 16-byte keys.");
    if (link) {
        add_value(chan_menu, chan_mem->uri);
    }
}

static void on_channel_delete(lv_event_t *e) {
    (void)e;
    if (chan_mem) {
        memset(chan_mem, 0, sizeof(*chan_mem));
        lichen_free(meshcore_ctx(), chan_mem);
    }
    chan_mem       = NULL;
    chan_menu      = chan_loading = chan_note = chan_scope_row = NULL;
    chan_group     = NULL;
    channel_screen = NULL;
}

static char pending_channel[MESH_CHANNEL_MAX + 1]; // what open_channel opens

static void info_start(void) {
    if (!chan_mem) {
        return;
    }
    job_t *job = job_new(JOB_CHANNEL_INFO, info_done);
    if (job) {
        snprintf(job->text, sizeof(job->text), "%s", chan_mem->name);
        job_start(job);
    } else if (job_running) {
        retry_later(info_start);
    }
}

static lv_obj_t *build_channel(void) {
    lv_obj_t *screen = shell_page_create();
    channel_screen   = screen;
    chan_group       = lv_group_get_default();
    lv_obj_add_event_cb(screen, on_channel_delete, LV_EVENT_DELETE, NULL);
    chan_mem  = lichen_alloc(meshcore_ctx(), sizeof(*chan_mem), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    chan_menu = menu_create_named(screen, "Channel", pending_channel);
    if (!chan_mem) {
        menu_note(chan_menu, "Not enough memory to show the channel.");
        return screen;
    }
    snprintf(chan_mem->name, sizeof(chan_mem->name), "%s", pending_channel);
    chan_note    = menu_note(chan_menu, "");
    chan_loading = menu_note(chan_menu, "Loading...");
    info_start();
    return screen;
}

static void open_channel(void *arg) {
    snprintf(pending_channel, sizeof(pending_channel), "%s", (char const *)arg);
    shell_open_keyed(build_channel, SCREEN_CHANNEL, shell_key_hash(pending_channel, strlen(pending_channel)));
}

static lv_obj_t *build_channels(void) {
    lv_obj_t *screen     = shell_page_create();
    channels_screen      = screen;
    lv_obj_t *menu       = menu_create(screen, "Channels");
    channel_rows[0]      = menu_add(menu, "Loading...", "", NULL, NULL);
    channel_row_count    = 1;
    add_tag_row          = menu_add(menu, "Add #hashtag channel", "", add_tag, NULL);
    menu_add(menu, "Add channel with key", "", add_keyed, NULL);
    channel_note = menu_note(menu, "A #hashtag channel's key comes from its name, as in MeshCore's apps.");
    lv_obj_add_event_cb(screen, on_channels_delete, LV_EVENT_DELETE, NULL);
    list_refresh();
    return screen;
}

static void open_channels(void *arg) {
    (void)arg;
    shell_open(build_channels, SCREEN_CHANNELS);
}
