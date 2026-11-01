// Settings > MeshCore (#67, settings.html): Name, Region & radio (MeshCore's presets, Custom..., Advanced: TX power),
// Channels (list, add a #hashtag, add with a key) and Send advert, then the about rows (meshcore_app.c).
// The mesh.h setters wait for the mesh lock (MeshCore may be saving contacts for a few hundred ms), so each change runs
// in a short-lived task on core 0, like the Wi-Fi scan; its result comes back to the LVGL task through lv_async_call
// and lands on whatever row or note is still on screen. The screens never wait on MeshCore.
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "menu.h"
#include "menu_filter.h"
#include "lichen_clock.h"
#include "mesh.h"
#include "mesh_chat.h"
#include "mesh_format.h"
#include "meshcore_settings.h"
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

#define SCREEN_RADIO    130
#define SCREEN_CUSTOM   131
#define SCREEN_CHANNELS 132
#define SCREEN_SWITCH   134
#define REMOVE_ASK_S    20 // the careful remove dialog cancels by itself after this
#define JOB_STACK       6144 // savePrefs / saveChannels keep File objects on the stack (the console's 4 KB suffices)
#define MAX_CHANNELS    40   // MAX_GROUP_CHANNELS in the mesh component's build
#define MAX_PRESETS     32
#define KEY_MAX         44   // a 32-byte key in base64
#define TX_SETTLE_MS    1000 // TX power is saved this long after the last step, to spare the flash
#define NAME_CHARS      31

// ---- the change, off the LVGL task -------------------------------------------------------------------------------

typedef enum { JOB_NAME, JOB_RADIO, JOB_CHANNEL_ADD, JOB_CHANNEL_RM, JOB_ADVERT, JOB_CHANNELS } job_kind_t;

typedef struct job {
    job_kind_t  kind;
    char        text[64];
    char        key[KEY_MAX + 1];
    float       freq, bw;
    uint8_t     sf, cr, path_hash_size;
    int8_t      tx_dbm;
    char        title[40]; // what is being applied, for the result line
    char const *reason;    // result: NULL = done
    bool        ok;
    size_t      n;
    char        names[MAX_CHANNELS][MESH_CHANNEL_MAX + 1];
    void (*done)(struct job *);
} job_t;

static bool job_running; // LVGL task only: one change at a time

static void job_finish(void *data) {
    job_t *job  = data;
    job_running = false;
    job->done(job);
    memset(job->key, 0, sizeof(job->key)); // a channel key: not left in the heap
    heap_caps_free(job);
}

static void job_task(void *arg) {
    job_t *job = arg;
    switch (job->kind) {
        case JOB_NAME: job->ok = mesh_set_name(job->text); break;
        case JOB_RADIO:
            job->reason = mesh_set_radio(job->freq, job->bw, job->sf, job->cr, job->path_hash_size, job->tx_dbm);
            break;
        case JOB_CHANNEL_ADD: job->reason = mesh_channel_add(job->text, job->key[0] ? job->key : NULL); break;
        case JOB_CHANNEL_RM: job->reason = mesh_channel_remove_named(job->text); break;
        case JOB_ADVERT: job->ok = mesh_send_advert(); break;
        case JOB_CHANNELS: job->n = mesh_channels_snapshot(job->names, MAX_CHANNELS); break;
    }
    // lv_async_call is not thread safe: take the LVGL lock like the shell does
    lvgl_port_lock(0);
    lv_async_call(job_finish, job);
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static job_t *job_new(job_kind_t kind, void (*done)(job_t *)) {
    if (job_running) {
        return NULL;
    }
    job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM);
    if (job) {
        job->kind = kind;
        job->done = done;
    }
    return job;
}

// false (job freed) when it could not start
static bool job_start(job_t *job) {
    if (xTaskCreatePinnedToCore(job_task, "meshset", JOB_STACK, job, 4, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "settings task did not start");
        heap_caps_free(job);
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

// The preset in use, as fw mesh names it (path hash size included); -1 for Custom
static int current_preset(mesh_settings_t const *s) {
    return radio_preset_match_exact(s->freq, s->bw, s->sf, s->cr, s->path_hash_size);
}

static void radio_values(char *out, size_t cap, float freq, float bw, uint8_t sf, uint8_t cr, bool marked) {
    snprintf(out, cap, "%s%.3f " LV_SYMBOL_BULLET " %g " LV_SYMBOL_BULLET " SF%u/CR%u",
             marked ? LV_SYMBOL_OK " " : "", freq, bw, sf, cr);
}

static void set_note(lv_obj_t *note, char const *text, bool problem) {
    if (note) {
        lv_label_set_text(note, text);
        lv_obj_set_style_text_color(note, lv_color_hex(problem ? THEME_AMBER : THEME_DIM), 0);
    }
}

// ---- the page ----------------------------------------------------------------------------------------------------

static lv_obj_t *name_row, *radio_row, *channels_row, *advert_row;

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
        menu_set_value(name_row, job->ok && mesh_get_settings(&s) ? s.name : "not saved (MeshCore busy?)");
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
            menu_set_value(name_row, "not saved: a name can't be empty");
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
            menu_set_value(name_row, "busy, try again");
        }
        return;
    }
    snprintf(job->text, sizeof(job->text), "%s", clean);
    if (!first_name_done) {
        menu_set_value(name_row, "saving...");
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

static void channels_line(job_t *job, lv_obj_t *row) {
    if (!row) {
        return;
    }
    if (job->n == MESH_BUSY) {
        menu_set_value(row, "busy");
        return;
    }
    char line[24];
    snprintf(line, sizeof(line), "%u channel%s", (unsigned)job->n, job->n == 1 ? "" : "s");
    menu_set_value(row, job->n ? line : "none");
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
        advert_status(!job->ok ? "not sent" : note ? "queued, approximate clock" : "advert queued");
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
        status("busy, try again");
        return;
    }
    advert_status = status;
    status("sending...");
    job_start(job);
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

static void on_page_delete(lv_event_t *e) {
    (void)e;
    name_row = radio_row = channels_row = advert_row = NULL;
    meshcore_advert_forget(advert_row_status);
}

void meshcore_settings(lv_obj_t *page) {
    mesh_settings_t s;
    if (!mesh_get_settings(&s)) {
        menu_note(page, "MeshCore is not running: its settings come back when it does.");
        return;
    }
    name_row     = menu_add(page, "Name", s.name, edit_name, NULL);
    radio_row    = menu_add(page, "Region & radio", "", open_radio, NULL);
    channels_row = menu_add(page, "Channels", "...", open_channels, NULL);
    advert_row   = menu_add(page, "Send advert", "", send_advert, NULL);
    lv_obj_add_event_cb(page, on_page_delete, LV_EVENT_DELETE, NULL);
    radio_row_refresh();
    page_channels_refresh();
}

// ---- Region & radio ----------------------------------------------------------------------------------------------

static lv_obj_t   *preset_rows[MAX_PRESETS];
static lv_obj_t   *custom_row, *tx_note;
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
        radio_values(text, sizeof(text), p->freq_mhz, p->bw_khz, p->sf, p->cr, (int)i == cur);
        menu_set_value(preset_rows[i], text);
    }
    if (cur < 0) {
        radio_values(text, sizeof(text), s.freq, s.bw, s.sf, s.cr, true);
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
    menu_note(menu, want.title);
    radio_values(text, sizeof(text), want.freq, want.bw, want.sf, want.cr, false);
    menu_note(menu, text);
    snprintf(text, sizeof(text), "Switch radio to %s?", want.title);
    menu_note(menu, text);
    if (want.level == RADIO_CONFIRM_BAND) {
        set_note(menu_note(menu, ""), "Another band than the current radio. It may not be legal where you are.", true);
    }
    menu_add(menu, "Switch radio", "Enter", switch_yes, NULL);
    menu_note(menu, "Esc keeps the current radio.");
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
        menu_set_value(preset_rows[i], "busy, try again");
        return;
    }
    job->freq           = p->freq_mhz;
    job->bw             = p->bw_khz;
    job->sf             = p->sf;
    job->cr             = p->cr;
    job->path_hash_size = p->path_hash_size;
    job->tx_dbm         = s.tx_dbm;
    job->n              = i; // the row for the result
    menu_set_value(preset_rows[i], "applying...");
    job_start(job);
}

static void pick_preset(void *arg) {
    size_t                i = (size_t)(uintptr_t)arg;
    radio_preset_t const *p = radio_preset_at(i);
    mesh_settings_t       s;
    if (!p || !mesh_get_settings(&s)) {
        return;
    }
    radio_confirm_t level = radio_confirm_needed(s.freq, s.bw, s.sf, s.cr, s.path_hash_size, p->freq_mhz, p->bw_khz,
                                                 p->sf, p->cr, p->path_hash_size);
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
    job->path_hash_size = s.path_hash_size;
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

static void on_radio_delete(lv_event_t *e) {
    (void)e;
    memset(preset_rows, 0, sizeof(preset_rows));
    custom_row = NULL;
    tx_note    = NULL;
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
    menu_note(menu, "Preset  (MHz " LV_SYMBOL_BULLET " kHz " LV_SYMBOL_BULLET " SF " LV_SYMBOL_BULLET " CR)");
    for (size_t i = 0; i < radio_preset_count() && i < MAX_PRESETS; i++) {
        preset_rows[i] = menu_add(menu, radio_preset_at(i)->title, "", pick_preset, (void *)(uintptr_t)i);
        menu_filter_add(filter, preset_rows[i], i);
    }
    custom_row = menu_add(menu, "Custom...", "", open_custom, NULL);
    menu_filter_fallback(filter, custom_row);
    menu_note(menu, "Advanced");
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
    snprintf(text, sizeof(text), "4/%u", draft_cr);
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
        set_note(custom_note, "Busy, try again", true);
        return;
    }
    job->freq           = draft_freq;
    job->bw             = radio_bandwidth_at(draft_bw);
    job->sf             = draft_sf;
    job->cr             = draft_cr;
    job->path_hash_size = s.path_hash_size;
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
    radio_confirm_t level = radio_confirm_needed(s.freq, s.bw, s.sf, s.cr, s.path_hash_size, draft_freq,
                                                 radio_bandwidth_at(draft_bw), draft_sf, draft_cr, s.path_hash_size);
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
    menu_note(menu, "Enter changes a value; nothing is used until Apply.");
    freq_row    = menu_add(menu, "Frequency", "", edit_freq, NULL);
    bw_row      = menu_add(menu, "Bandwidth", "", next_bw, NULL);
    sf_row      = menu_add(menu, "Spreading factor", "", next_sf, NULL);
    cr_row      = menu_add(menu, "Coding rate", "", next_cr, NULL);
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
static char      row_names[MAX_CHANNELS][MESH_CHANNEL_MAX + 1]; // what Enter on a channel row removes
static void      remove_ask(void *arg);

static void list_done(job_t *job) {
    channels_line(job, channels_row);
    if (!add_tag_row) {
        return; // the screen is gone
    }
    lv_obj_t   *menu     = lv_obj_get_parent(add_tag_row);
    lv_group_t *previous = lv_group_get_default();
    lv_group_t *group = lv_obj_get_group(add_tag_row);
    lv_group_set_default(group); // new rows join the screen's group
    // Where the selection was, so a removal leaves it on the next channel instead of jumping to the last row
    size_t keep = 0;
    for (size_t i = 0; i < channel_row_count; i++) {
        if (channel_rows[i] == lv_group_get_focused(group)) {
            keep = i;
        }
    }
    for (size_t i = 0; i < channel_row_count; i++) {
        lv_obj_delete(channel_rows[i]);
    }
    channel_row_count = 0;
    size_t n          = job->n == MESH_BUSY ? 0 : job->n;
    for (size_t i = 0; i < n; i++) {
        snprintf(row_names[i], sizeof(row_names[i]), "%s", job->names[i]);
        // The first channel is Public: nothing to remove there
        channel_rows[channel_row_count++] = menu_add(menu, job->names[i], "", i ? remove_ask : NULL, row_names[i]);
    }
    if (!n) {
        channel_rows[channel_row_count++] = menu_add(menu, job->n == MESH_BUSY ? "MeshCore busy" : "No channels", "",
                                                     NULL, NULL);
    }
    lv_group_set_default(previous);
    // After the heading, before the add rows
    for (size_t i = 0; i < channel_row_count; i++) {
        if (channel_rows[i]) {
            lv_obj_move_to_index(channel_rows[i], (int32_t)(1 + i));
        }
    }
    lv_obj_t *focus = channel_rows[keep < channel_row_count ? keep : channel_row_count - 1];
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
        set_note(channel_note, "Busy, try again", true);
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
        if (add_tag_row) {
            list_refresh();
        } else {
            page_channels_refresh();
        }
    }
}

// Runs on the channel list, after the confirm dialog
static void remove_go(void) {
    job_t *job = job_new(JOB_CHANNEL_RM, remove_done);
    if (!job) {
        set_note(channel_note, "Busy, try again", true);
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
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: remove · Esc: keep it",
        .seconds      = REMOVE_ASK_S,
        .careful      = true,
    };
    snprintf(title, sizeof(title), "Remove %s? Messages already received stay.", remove_name);
    if (!shell_dialog_countdown(&d, remove_answer)) {
        set_note(channel_note, "Busy, try again", true);
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
}

static lv_obj_t *build_channels(void) {
    lv_obj_t *screen     = shell_page_create();
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
