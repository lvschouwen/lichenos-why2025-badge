// Crash log (#72): the crashes Lichen recorded, newest first, through lichen_crashlog.h. Enter opens one full screen
// (Up/Down scroll, Enter deletes it); "Clear all" empties the log, the crash counter and the core dump like
// "fw crash clear". Both ask first with a careful dialog: the countdown cancels and Enter needs a fresh press. The tile
// counts the entries new since the app was last opened, and after a crash home offers to open it once.
// A delete or a clear writes flash and NVS: it runs on a short-lived task on core 0 (as sd_screen.c's jobs do) and the
// outcome comes back through lv_async_call; the list's 1 s timer collects it too, should that call have failed.
#include "app_icons.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fonts.h"
#include "lichen_app.h"
#include "lichen_crashlog.h"
#include "lichen_ui.h"
#include "menu.h"
#include "nav.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "crashlog";

#define SCREEN_CRASH_ENTRY 107
#define ASK_SECONDS        20
#define NOTICE_SECONDS     30
#define SCROLL_STEP        96
#define NOTICE_TICK_MS     1000
#define NOTICE_SETTLE      2 // ticks home must be on top and idle: first-run setup opens over it shortly after boot
#define FULL_CAP           1536
#define LINE_CAP           64
#define RESULT_CAP         96
#define JOB_STACK          6144
#define POLL_MS            1000
#define JOB_PENDING        (-1)

static lichen_app_t const crashlog_app;

// The open viewer; all NULL/0 while it is closed. LVGL task only.
static struct {
    lv_obj_t         *screen; // the app's screen with the list
    lv_group_t       *group;  // its keypad group, for the rows of a rebuilt list
    lv_obj_t         *menu;
    crashlog_entry_t *entries; // PSRAM, CRASHLOG_MAX
    size_t            count;
    size_t            focus; // the row to select after a rebuild
    size_t            shown; // the entry on the full screen (and the one a delete question is about)
    lv_obj_t         *clear_row;
    lv_obj_t         *hint;  // the full screen's key hint, NULL while it is closed
    lv_timer_t       *poll;
} s_v;

// The running delete or clear; it outlives the viewer. s_job and s_result: LVGL task only.
typedef enum { JOB_NONE, JOB_DELETE, JOB_CLEAR } job_t;
static job_t            s_job;
static atomic_int       s_job_outcome = JOB_PENDING; // the task stores 0/1, the LVGL task takes it
static size_t           s_job_index;
static crashlog_entry_t s_job_expect;                // the job's copy: the list may be rebuilt meanwhile
static char             s_result[RESULT_CAP];        // the last job's outcome, shown above the list

// The tile's lines, read again only when the log changed or the viewer marked it seen
static struct {
    bool     valid;
    uint32_t generation;
    char     l1[LINE_CAP], l2[LINE_CAP];
} s_tile;

static lichen_crashlog_status_t s_status; // too big for the LVGL task's stack

// ---- the list -----------------------------------------------------------------------------------------------------

static void on_entry(void *arg);
static void on_clear_all(void *arg);

static void add_rows(void) {
    if (s_result[0]) {
        lv_obj_set_style_text_color(menu_note(s_v.menu, s_result), lv_color_hex(THEME_AMBER), 0);
    }
    for (size_t i = 0; i < s_v.count; i++) {
        char title[LINE_CAP], task[24];
        crashlog_row(&s_v.entries[i], title, sizeof(title), task, sizeof(task));
        menu_add(s_v.menu, title, task, on_entry, (void *)(intptr_t)i);
    }
    if (s_v.count) {
        s_v.clear_row = menu_add(s_v.menu, "Clear all", NULL, on_clear_all, NULL);
        menu_note(s_v.menu, "Enter opens an entry. Clear all also resets the crash counter and erases the core dump.");
    } else {
        menu_note(s_v.menu, s_v.entries ? "No crashes stored." : "Not enough memory to show the crash log.");
    }
}

// (Re)builds the menu from the log; rows join the screen's own group, whatever group is the default now
static void fill_list(void) {
    if (!s_v.screen) {
        return;
    }
    if (s_v.menu) {
        lv_obj_delete(s_v.menu);
    }
    s_v.clear_row = NULL;
    s_v.count = s_v.entries ? lichen_crashlog_list(s_v.entries, CRASHLOG_MAX) : 0;
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(s_v.group);
    s_v.menu = menu_create(s_v.screen, "Crash log");
    add_rows();
    lv_group_set_default(previous);
    // Without rows the screen itself takes the keys (Esc); with rows it must not be a stop of its own
    bool rows = s_v.count > 0;
    if (!rows && lv_obj_get_group(s_v.screen) != s_v.group) {
        lv_group_add_obj(s_v.group, s_v.screen);
    } else if (rows && lv_obj_get_group(s_v.screen) == s_v.group) {
        lv_group_remove_obj(s_v.screen);
    }
    if (rows) {
        menu_focus_row(s_v.menu, (int)(s_v.focus < s_v.count ? s_v.focus : s_v.count - 1));
    }
}

static void refill(void *arg) {
    (void)arg;
    fill_list();
}

// ---- delete and clear, off the LVGL task ---------------------------------------------------------------------------

// From job_done or the list's poll timer, whichever comes first; the other finds nothing
static void job_collect(void) {
    int outcome = atomic_exchange(&s_job_outcome, JOB_PENDING);
    if (outcome == JOB_PENDING || s_job == JOB_NONE) {
        return;
    }
    job_t kind   = s_job;
    s_job        = JOB_NONE;
    s_tile.valid = false;
    if (kind == JOB_DELETE) {
        snprintf(s_result, sizeof(s_result), "%s",
                 outcome ? "Entry deleted." : "Entry was not deleted: the log changed or could not be written.");
    } else {
        snprintf(s_result, sizeof(s_result), "%s",
                 outcome ? "Cleared: the log, the crash counter and the core dump." : "Cleared, but erasing the core dump failed.");
    }
    ESP_LOGW(TAG, "%s", s_result);
    s_v.focus = kind == JOB_DELETE ? s_job_index : 0;
    if (s_v.screen) {
        shell_back_to(s_v.screen, refill, NULL); // the entry's screen goes if it is open, then the list is read again
    }
}

static void job_done(void *arg) {
    (void)arg;
    job_collect();
}

static void job_task(void *arg) {
    bool ok = (job_t)(intptr_t)arg == JOB_CLEAR ? lichen_crashlog_clear(&crashlog_app)
                                                : lichen_crashlog_delete(&crashlog_app, s_job_index, &s_job_expect);
    ESP_LOGW(TAG, "job stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    atomic_store(&s_job_outcome, ok ? 1 : 0);
    // lv_async_call is not thread safe: take the LVGL lock like the shell does
    lvgl_port_lock(0);
    lv_async_call(job_done, NULL);
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static bool job_start(job_t kind) {
    if (s_job != JOB_NONE) {
        return false;
    }
    s_job       = kind;
    s_result[0] = '\0';
    atomic_store(&s_job_outcome, JOB_PENDING);
    if (xTaskCreatePinnedToCore(job_task, "crashjob", JOB_STACK, (void *)(intptr_t)kind, 4, NULL, 0) != pdPASS) {
        s_job = JOB_NONE;
        snprintf(s_result, sizeof(s_result), "Not started: out of memory.");
        ESP_LOGW(TAG, "%s", s_result);
        return false;
    }
    return true;
}

static void on_poll(lv_timer_t *timer) {
    (void)timer;
    job_collect();
}

// ---- the questions ------------------------------------------------------------------------------------------------

static void clear_answer(bool yes) {
    if (!yes || !s_v.screen) {
        return;
    }
    if (job_start(JOB_CLEAR)) {
        if (s_v.clear_row) {
            menu_set_value(s_v.clear_row, "Clearing...");
        }
    } else {
        lv_async_call(refill, NULL); // shows why; not from inside the dialog's key handling
    }
}

static void on_clear_all(void *arg) {
    (void)arg;
    static char title[96];
    if (s_job != JOB_NONE) {
        return;
    }
    snprintf(title, sizeof(title), "Clear all %u crash entries, the crash counter and the core dump?", (unsigned)s_v.count);
    shell_dialog_t const d = {
        .title        = title,
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: clear all \xE2\x80\xA2 Esc: keep",
        .seconds      = ASK_SECONDS,
        .careful      = true,
    };
    shell_dialog_countdown(&d, clear_answer);
}

// ---- one entry, full screen ---------------------------------------------------------------------------------------

static void delete_answer(bool yes) {
    if (!yes || !s_v.screen || s_v.shown >= s_v.count) {
        return;
    }
    s_job_index  = s_v.shown;
    s_job_expect = s_v.entries[s_v.shown];
    if (job_start(JOB_DELETE)) {
        if (s_v.hint) {
            lv_label_set_text(s_v.hint, "Deleting...");
        }
    } else {
        shell_back_to(s_v.screen, refill, NULL); // the list says why
    }
}

static void ask_delete(void) {
    if (s_job != JOB_NONE) {
        return;
    }
    shell_dialog_t const d = {
        .title        = "Delete this crash entry? The core dump stays; Clear all erases it.",
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: delete \xE2\x80\xA2 Esc: keep",
        .seconds      = ASK_SECONDS,
        .careful      = true,
    };
    shell_dialog_countdown(&d, delete_answer);
}

static void on_entry_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_target(e), key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
    } else if (key == LV_KEY_ENTER) {
        ask_delete();
    }
}

static lv_obj_t *add_text(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static void on_entry_delete(lv_event_t *e) {
    (void)e;
    s_v.hint = NULL;
}

static lv_obj_t *build_entry(void) {
    if (!s_v.screen || s_v.shown >= s_v.count) {
        return NULL;
    }
    static char             full[FULL_CAP];
    crashlog_entry_t const *e      = &s_v.entries[s_v.shown];
    lv_obj_t               *screen = shell_page_create();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 16, 0);
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);
    shell_scroll_tame(screen);
    lv_obj_t *title = add_text(screen, e->parsed ? crashlog_reason_text(e->reason) : "Crash log entry", THEME_TEXT);
    lv_obj_set_style_text_font(title, lichen_font(LICHEN_FONT_LARGE), 0);
    crashlog_full(e, s_v.shown == 0, full, sizeof(full));
    add_text(screen, full, THEME_TEXT);
    s_v.hint = add_text(screen, "Up/Down: scroll \xE2\x80\xA2 Enter: delete \xE2\x80\xA2 Esc: back", THEME_AMBER);
    lv_obj_add_event_cb(screen, on_entry_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(screen, on_entry_delete, LV_EVENT_DELETE, NULL);
    return screen;
}

static void on_entry(void *arg) {
    s_v.shown = (size_t)(intptr_t)arg;
    s_v.focus = s_v.shown;
    shell_open(build_entry, SCREEN_CRASH_ENTRY);
}

// ---- the app ------------------------------------------------------------------------------------------------------

static void on_screen_delete(lv_event_t *e) {
    (void)e;
    lv_timer_delete(s_v.poll);
    heap_caps_free(s_v.entries);
    memset(&s_v, 0, sizeof(s_v));
}

static void open_crashlog(lv_obj_t *screen) {
    memset(&s_v, 0, sizeof(s_v));
    s_v.screen  = screen;
    s_v.group   = lv_group_get_default(); // shell_open's group for this screen while it is built
    s_v.entries = heap_caps_calloc(CRASHLOG_MAX, sizeof(crashlog_entry_t), MALLOC_CAP_SPIRAM);
    lichen_crashlog_mark_seen(&crashlog_app); // logs once when it cannot
    s_tile.valid = false;
    if (s_job == JOB_NONE) {
        s_result[0] = '\0'; // an old outcome is news no more
    }
    s_v.poll = lv_timer_create(on_poll, POLL_MS, NULL);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    fill_list();
    lv_obj_add_event_cb(screen, on_screen_delete, LV_EVENT_DELETE, NULL);
}

static bool tile_text(char *l1, size_t c1, char *l2, size_t c2) {
    uint32_t generation = lichen_crashlog_generation();
    if (!s_tile.valid || generation != s_tile.generation) {
        if (!lichen_crashlog_status(&crashlog_app, &s_status)) {
            return false; // not read (no memory): keep what is shown, try again in a second
        }
        crashlog_tile(s_status.entries, s_status.unseen, &s_status.newest, s_tile.l1, sizeof(s_tile.l1), s_tile.l2,
                      sizeof(s_tile.l2));
        s_tile.generation = generation;
        s_tile.valid      = true;
    }
    snprintf(l1, c1, "%s", s_tile.l1);
    snprintf(l2, c2, "%s", s_tile.l2);
    return true;
}

static lichen_app_t const crashlog_app = {
    .name      = "Crash log",
    .id        = "crashlog",
    .draw_icon = icon_crashlog,
    .tile_text = tile_text,
    .open      = open_crashlog,
};

// ---- after a crash: one question on home ---------------------------------------------------------------------------

static void open_viewer(void *arg) {
    (void)arg;
    for (size_t i = 0; i < lichen_app_count(); i++) {
        if (lichen_app_at(i) == &crashlog_app) {
            shell_open_app(i);
        }
    }
}

static void notice_answer(bool yes) {
    if (yes) {
        lv_async_call(open_viewer, NULL);
    }
}

// Once home is on top for a moment, with no dialog and no setup screen over it
static void notice_tick(lv_timer_t *timer) {
    static int idle;
    idle = (shell_home_shown() && nav_depth() == 1 && !shell_dialog_group()) ? idle + 1 : 0;
    if (idle < NOTICE_SETTLE) {
        return;
    }
    lv_timer_delete(timer);
    shell_dialog_t const d = {
        .title        = "Lichen restarted after a crash. Open the crash log?",
        .count_prefix = "(closes in ",
        .count_suffix = " s)",
        .hint         = "Enter: open \xE2\x80\xA2 Esc: later",
        .seconds      = NOTICE_SECONDS,
        .careful      = true, // the countdown closes it instead of opening the viewer
    };
    ESP_LOGW(TAG, "after a crash: asking to open the crash log");
    shell_dialog_countdown(&d, notice_answer);
}

void crashlog_app_register(void) {
    lichen_register_app(&crashlog_app);
    if (lichen_crashlog_after_crash()) {
        lvgl_port_lock(0);
        lv_timer_create(notice_tick, NOTICE_TICK_MS, NULL);
        lvgl_port_unlock();
    }
}
