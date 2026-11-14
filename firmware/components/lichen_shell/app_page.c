// Settings > Apps (#233): the OS's rows on an app's page. Everything that touches a drive or the settings store runs on
// app_data_job's task and answers on the page's LVGL timer; the LVGL task only builds rows and sets texts.
// UI standard: R1 (the heading names the app, app_settings.c), R2 (text from the model, wrapped by the menu), R3 (the
// state is the row's value; Background is a toggle, Clear data a careful-dialog row), R4 (sizes and counts in words),
// R5 (Reading..., Clearing... end in a final state), R6 (focus lands on the first useful row, survives a refresh), R9
// (Clear data, Background: one word each), R10 (the switch answers at once), R11 (No card, None).
#include "app_page.h"

#include "app_data_job.h"
#include "app_page_model.h"
#include "esp_log.h"
#include "lichen_sys_app.h"
#include "lichen_sys_life.h"
#include "lichen_ui.h"
#include "menu.h"

#include <string.h>

static char const TAG[] = "app_page";

#define POLL_MS 150

// One page at a time (an app's page opens from the Settings list, never from another app page)
static struct {
    lv_obj_t   *menu;
    lv_obj_t   *data_row, *sd_row, *bg_row, *crash_row, *clear_row;
    size_t      index;
    app_page_rows_t rows;
} s_pg;

static app_page_app_t describe(lichen_app_t const *app) {
    return (app_page_app_t){
        .caps           = app->caps,
        .has_hooks      = app->settings || app->about,
        .has_background = app->start_background != NULL,
        .exempt         = app->safe_mode_exempt,
        .secret_storage = app->storage_secret,
    };
}

bool app_page_listed(size_t index) {
    lichen_app_t const *app = lichen_app_at(index);
    if (!app) {
        return false;
    }
    app_page_app_t d = describe(app);
    return app_page_rows(&d).listed;
}

// ---- the rows' values (read the RAM records only: no I/O) ----------------------------------------------------------

static void refresh_background(void) {
    if (!s_pg.bg_row) {
        return;
    }
    app_life_t life = {0};
    lichen_sys_life_get(s_pg.index, &life);
    unsigned window;
    unsigned crashes = lichen_sys_app_crashes(s_pg.index, &window);
    char     text[32];
    app_page_bg_text(s_pg.rows.bg, lichen_sys_app_background_allowed(s_pg.index), app_page_bg_running(&life),
                     life.fault == LIFE_FAULT_CRASHES && crashes > 0, text, sizeof(text));
    menu_set_value(s_pg.bg_row, text);
}

static void refresh_crashes(void) {
    if (!s_pg.crash_row) {
        return;
    }
    unsigned window;
    unsigned crashes = lichen_sys_app_crashes(s_pg.index, &window);
    char     text[40];
    app_page_crash_text(crashes, window, text, sizeof(text));
    menu_set_value(s_pg.crash_row, text);
}

static void show_data(lichen_app_data_t const *d) {
    char text[24];
    app_page_data_text(d, text, sizeof(text));
    menu_set_value(s_pg.data_row, text);
    app_page_sd_text(d, text, sizeof(text));
    menu_set_value(s_pg.sd_row, text);
}

// ---- jobs ----------------------------------------------------------------------------------------------------------

static void finish_clear(adj_result_t const *r) {
    app_life_t life = {0};
    lichen_sys_life_get(s_pg.index, &life);
    app_page_clear_msg_t m;
    app_page_clear_msg(&r->clear, app_page_needs_restart(&life), &m);
    if (m.ok) {
        menu_status_done(s_pg.menu, m.text);
    } else {
        menu_status_fail(s_pg.menu, m.text, m.next);
    }
    if (app_data_job_start(ADJ_MEASURE, s_pg.index, false)) { // the rows show what is left
        return;
    }
    menu_set_value(s_pg.data_row, "Unknown");
}

static void finish_background(adj_result_t const *r) {
    if (!r->ok) {
        menu_status_fail(s_pg.menu, "The switch could not be saved", "It goes back at the next restart.");
    }
    refresh_background();
}

static void poll(lv_timer_t *t) {
    (void)t;
    adj_result_t r;
    if (!s_pg.menu || !app_data_job_take(&r) || r.index != s_pg.index) {
        return; // a job of a page that is gone: its result is dropped
    }
    if (r.kind == ADJ_MEASURE) {
        show_data(&r.data);
        if (!r.ok) {
            menu_set_value(s_pg.data_row, "Partly unknown");
        }
    } else if (r.kind == ADJ_CLEAR) {
        finish_clear(&r);
    } else {
        finish_background(&r);
    }
    refresh_crashes();
}

static void forget_page(lv_event_t *e) {
    if (lv_event_get_user_data(e)) {
        lv_timer_delete(lv_event_get_user_data(e));
    }
    memset(&s_pg, 0, sizeof(s_pg));
}

// A second job cannot start while one runs: say so in words (R5) instead of ignoring the key (R10)
static bool start_job(adj_kind_t kind, bool allowed) {
    if (app_data_job_start(kind, s_pg.index, allowed)) {
        return true;
    }
    menu_status_fail(s_pg.menu, "Still busy with the last change", "Try again in a moment.");
    return false;
}

// ---- row actions ---------------------------------------------------------------------------------------------------

static void toggle_background(void *arg) {
    (void)arg;
    bool allowed = !lichen_sys_app_background_allowed(s_pg.index);
    if (!start_job(ADJ_BACKGROUND, allowed)) {
        return;
    }
    // The answer is at once (R10); the job saves it and the row is read back from the list when it ends
    app_life_t life = {0};
    lichen_sys_life_get(s_pg.index, &life);
    char text[32];
    app_page_bg_text(s_pg.rows.bg, allowed, app_page_bg_running(&life), false, text, sizeof(text));
    menu_set_value(s_pg.bg_row, text);
}

static void clear_data(void *arg) {
    (void)arg; // runs only after the careful dialog said yes
    menu_status_begin(s_pg.menu, "Clearing");
    if (!app_data_job_start(ADJ_CLEAR, s_pg.index, false)) {
        menu_status_fail(s_pg.menu, "Could not start", "Try again in a moment.");
        return;
    }
}

// ---- the page ------------------------------------------------------------------------------------------------------

static lv_obj_t *info_row(lv_obj_t *menu, char const *label, char const *value) {
    lv_obj_t *row = menu_add(menu, label, value, NULL, NULL);
    menu_set_kind(row, MENU_INFO);
    return row;
}

static void add_background_row(lv_obj_t *menu) {
    s_pg.bg_row = menu_add(menu, "Background", "", s_pg.rows.bg == APP_BG_SWITCH ? toggle_background : NULL, NULL);
    menu_set_kind(s_pg.bg_row, s_pg.rows.bg == APP_BG_SWITCH ? MENU_TOGGLE : MENU_INFO);
    refresh_background();
}

static void add_clear_row(lv_obj_t *menu, lichen_app_t const *app) {
    char question[160];
    app_page_clear_question(app->name, question, sizeof(question));
    s_pg.clear_row = menu_add(menu, "Clear data", "", clear_data, NULL);
    menu_set_danger(s_pg.clear_row, question, "clear data");
}

// Its own frame: the words are up to ~250 characters and the label copies them
static void add_caps_note(lv_obj_t *menu, lichen_app_t const *app) {
    char caps[APP_PAGE_CAPS_MAX];
    app_page_caps_text(app->caps, caps, sizeof(caps));
    menu_note(menu, caps);
}

void app_page_add_rows(lv_obj_t *screen, lv_obj_t *menu, size_t index) {
    lichen_app_t const *app = lichen_app_at(index);
    if (!app) {
        return;
    }
    memset(&s_pg, 0, sizeof(s_pg));
    s_pg.menu  = menu;
    s_pg.index = index;
    app_page_app_t d = describe(app);
    s_pg.rows        = app_page_rows(&d);
    menu_note(menu, "Managed by Lichen");
    add_caps_note(menu, app);
    lv_timer_t *timer = lichen_ui_timer_create(poll, POLL_MS, NULL);
    lichen_ui_event_add(screen, forget_page, LV_EVENT_DELETE, timer);
    if (!timer) { // no way to hear back from a job: show only what needs none, and say why
        menu_note(menu, "Out of memory: Data, the Background switch and Clear data are not available. Restart the badge.");
        s_pg.rows.data = s_pg.rows.background = s_pg.rows.clear = false;
        if (s_pg.rows.crashes) {
            s_pg.crash_row = info_row(menu, "Crashes", "");
            refresh_crashes();
        }
        return;
    }
    if (s_pg.rows.data) {
        s_pg.data_row = info_row(menu, "Data", "Reading...");
        s_pg.sd_row   = info_row(menu, "On the SD card", "Reading...");
    }
    if (s_pg.rows.background) {
        add_background_row(menu);
    }
    if (s_pg.rows.crashes) {
        s_pg.crash_row = info_row(menu, "Crashes", "");
        refresh_crashes();
    }
    if (s_pg.rows.clear) {
        add_clear_row(menu, app);
    } else if (s_pg.rows.clear_note) {
        menu_note(menu, "Its identity and keys are erased only by Factory reset.");
    }
    if (s_pg.rows.data && !app_data_job_start(ADJ_MEASURE, index, false)) {
        menu_set_value(s_pg.data_row, "Unknown");
        menu_set_value(s_pg.sd_row, "Unknown");
        ESP_LOGW(TAG, "%s: the size could not be read (a job is running)", app->id ? app->id : app->name);
    }
}
