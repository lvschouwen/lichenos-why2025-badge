// Notes (#154): short notes and to dos, one plain text file each in the app's folder on the SD card (lichen_fs_sd_dir),
// so they survive a restart and a computer can read them. A to do is a note whose line starts with "[ ] " or "[x] ".
// The file work (notes_store.c, plain POSIX) runs on a short-lived task on core 0 with the card's batch lock held; the
// screens poll for its result with an LVGL timer (as app_crashlog.c does). The rules are in notes_model.c.
#include "esp_log.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_app.h"
#include "lichen_fs.h"
#include "lichen_mem.h"
#include "lichen_ui.h"
#include "lichen_task.h"
#include "menu.h"
#include "notes_model.h"
#include "notes_store.h"
#include "screen_ids.h"
#include "lichen_apps.h"
#include "text_entry.h"
#include "theme.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "notes";

#define MAX_NOTES   100
#define JOB_STACK   8192
#define SD_WAIT_MS  3000
#define POLL_MS     200
#define RESULT_CAP  96
#define LONG_BODY   50 // longer than this, the note's page shows the whole text under the heading
#define FIRST_NOTE_ROW 2 // "New note..." and "New to do..." come first

static lichen_app_t const notes_list_app;

typedef enum { JOB_LOAD, JOB_ADD, JOB_WRITE, JOB_DELETE } job_kind_t;
typedef enum { ST_IDLE, ST_RUNNING, ST_DONE } job_state_t;

// The job's request and result; the task owns them while s_state is RUNNING, the LVGL task otherwise
static struct {
    job_kind_t kind;
    char       name[NOTES_NAME_MAX]; // WRITE, DELETE: the note; ADD: gets the new note's name
    char       line[NOTES_LINE_MAX];
} s_req;
static struct {
    notes_result_t result;   // of the add, write or delete
    bool           no_card;  // no card, or its lock was not free
    bool           listed;   // the list below is current
    size_t         count, skipped;
} s_res;
static atomic_int s_state = ST_IDLE;

// The open list; NULL/0 while it is closed. LVGL task only. items is written by the job task while it runs.
static struct {
    lv_obj_t     *screen;
    lv_group_t   *group;
    lv_timer_t   *poll;
    lv_obj_t     *menu;
    notes_item_t *items; // PSRAM, MAX_NOTES; kept for the life of the badge once made
    size_t        count, skipped;
    bool          no_card;
    bool          busy;
    char          busy_text[24];
    char          result[RESULT_CAP]; // the last job's outcome, shown above the rows
    bool          result_bad;
    char          focus_name[NOTES_NAME_MAX]; // the note to select after a rebuild
    size_t        focus_index;               // when no name: the row after a delete
    size_t        shown;                     // the note on its page
} s_v;

static unsigned s_open_todos; // for the home tile
static bool     s_counted;

// ---- the job ------------------------------------------------------------------------------------------------------

static void run_op(char const *dir) {
    switch (s_req.kind) {
    case JOB_LOAD:   s_res.result = NOTES_OK; break;
    case JOB_ADD:    s_res.result = notes_store_add(dir, s_req.line, s_req.name, sizeof(s_req.name)); break;
    case JOB_WRITE:  s_res.result = notes_store_write(dir, s_req.name, s_req.line); break;
    case JOB_DELETE: s_res.result = notes_store_delete(dir, s_req.name); break;
    }
}

static void job_task(void *arg) {
    (void)arg;
    memset(&s_res, 0, sizeof(s_res));
    char const *dir = lichen_fs_sd_dir(&notes_list_app); // before the batch: it takes the card's lock itself
    if (!dir || !lichen_fs_sd_begin(&notes_list_app, SD_WAIT_MS)) {
        s_res.no_card = true;
        s_res.result  = NOTES_ERR_OPEN;
    } else {
        errno = 0;
        run_op(dir);
        int op_err = s_res.result == NOTES_ERR_IO ? errno : 0; // only a failed write can be the card's fault
        errno      = 0;
        s_res.listed = notes_store_list(dir, s_v.items, MAX_NOTES, &s_res.count, &s_res.skipped) == NOTES_OK;
        int err      = op_err ? op_err : s_res.listed ? 0 : errno;
        lichen_fs_sd_end(&notes_list_app, err);
    }
    ESP_LOGI(TAG, "job %d -> %d, %u notes, stack margin %u", (int)s_req.kind, (int)s_res.result, (unsigned)s_res.count,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    atomic_store(&s_state, ST_DONE);
}

static bool job_start(void) {
    int expected = ST_IDLE;
    if (!s_v.items || !atomic_compare_exchange_strong(&s_state, &expected, ST_RUNNING)) {
        return false;
    }
    if (!lichen_task_start(&notes_list_app, "notesjob", job_task, NULL, JOB_STACK, LICHEN_PRIO_JOB, NULL)) {
        ESP_LOGW(TAG, "no memory for a notes job");
        atomic_store(&s_state, ST_IDLE);
        return false;
    }
    return true;
}

// ---- the list -----------------------------------------------------------------------------------------------------

static void on_item(void *arg);
static void ask_new(void *arg);

static void set_result(char const *text, bool bad) {
    snprintf(s_v.result, sizeof(s_v.result), "%s", text);
    s_v.result_bad = bad;
}

static char const *value_text(note_kind_t kind) {
    return kind == NOTE_TODO ? "To do" : kind == NOTE_DONE ? "Done" : "";
}

static void add_rows(lv_obj_t *menu) {
    if (s_v.result[0]) {
        lv_obj_set_style_text_color(menu_note(menu, s_v.result), lv_color_hex(s_v.result_bad ? THEME_RED : THEME_TEAL), 0);
    }
    if (s_v.busy) {
        menu_note(menu, s_v.busy_text);
        return;
    }
    if (s_v.no_card) {
        menu_note(menu, "No SD card: notes are saved on the card. Insert one, then open Notes again.");
        return;
    }
    menu_add(menu, "New note...", NULL, ask_new, (void *)(intptr_t)NOTE_PLAIN);
    menu_add(menu, "New to do...", NULL, ask_new, (void *)(intptr_t)NOTE_TODO);
    for (size_t i = 0; i < s_v.count; i++) {
        char const *body;
        note_kind_t kind = notes_parse(s_v.items[i].line, &body);
        menu_add(menu, body, value_text(kind), on_item, (void *)(intptr_t)i);
    }
    if (!s_v.count) {
        menu_empty(menu, "notes", "choose New note to write one.");
    }
    if (s_v.skipped) {
        char text[64];
        snprintf(text, sizeof(text), "%u files not shown (empty, or more than %d notes).", (unsigned)s_v.skipped, MAX_NOTES);
        menu_note(menu, text);
    }
}

static int focus_row(void) {
    if (s_v.focus_name[0]) {
        for (size_t i = 0; i < s_v.count; i++) {
            if (strcmp(s_v.items[i].name, s_v.focus_name) == 0) {
                return (int)(FIRST_NOTE_ROW + i);
            }
        }
    }
    if (s_v.focus_index && s_v.count) {
        return (int)(FIRST_NOTE_ROW + (s_v.focus_index < s_v.count ? s_v.focus_index : s_v.count - 1));
    }
    return 0;
}

// lichen_ui_with_group's build: the menu and its rows, into the screen's group
static void build_menu(void *arg) {
    (void)arg;
    s_v.menu = menu_create(s_v.screen, "Notes");
    add_rows(s_v.menu);
}

// (Re)builds the menu; rows join the screen's own group, whatever group is the default now
static void fill_list(void) {
    if (!s_v.screen) {
        return;
    }
    if (s_v.menu) {
        lv_obj_delete(s_v.menu);
    }
    lichen_ui_with_group(s_v.group, build_menu, NULL);
    bool rows = !s_v.busy && !s_v.no_card;
    // Without rows the screen itself takes the keys (Esc); with rows it must not be a stop of its own
    if (!rows && lv_obj_get_group(s_v.screen) != s_v.group) {
        lv_group_add_obj(s_v.group, s_v.screen);
    } else if (rows && lv_obj_get_group(s_v.screen) == s_v.group) {
        lv_group_remove_obj(s_v.screen);
    }
    if (rows) {
        menu_focus_row(s_v.menu, focus_row());
        shell_page_hint(s_v.screen, "Enter", "open", NULL);
    }
}

static void refill(void *arg) {
    (void)arg;
    fill_list();
}

// Back to the list (an item's page or the entry may be open over it), then redraw it
static void show_list(void) {
    if (s_v.screen && !shell_back_to(s_v.screen, refill, NULL)) {
        fill_list();
    }
}

static void count_todos(void) {
    unsigned n = 0;
    for (size_t i = 0; i < s_v.count; i++) {
        char const *body;
        n += notes_parse(s_v.items[i].line, &body) == NOTE_TODO;
    }
    s_open_todos = n;
    s_counted    = true;
}

static void job_collect(void) {
    int expected = ST_DONE;
    if (!atomic_compare_exchange_strong(&s_state, &expected, ST_IDLE)) {
        return;
    }
    s_v.busy    = false;
    s_v.no_card = s_res.no_card;
    if (s_res.listed) {
        s_v.count   = s_res.count;
        s_v.skipped = s_res.skipped;
        count_todos();
    } else {
        s_v.count = s_v.skipped = 0;
    }
    if (s_res.no_card || s_req.kind == JOB_LOAD) {
        s_v.result[0] = '\0';
    } else if (s_res.result == NOTES_OK) {
        set_result(s_req.kind == JOB_DELETE ? "Deleted." : "Saved.", false);
    } else {
        set_result(notes_result_text(s_res.result), true);
    }
    if (!s_res.no_card && !s_res.listed) {
        set_result("The notes could not be read from the SD card.", true);
    }
    snprintf(s_v.focus_name, sizeof(s_v.focus_name), "%s", s_req.kind == JOB_DELETE ? "" : s_req.name);
    show_list();
}

static void on_poll(lv_timer_t *timer) {
    (void)timer;
    job_collect();
}

// Starts the job in s_req and shows the list busy until it is done (R5, R10)
static void begin_job(char const *busy_text) {
    set_result("", false);
    if (!job_start()) {
        set_result("Not started: the SD card is busy. Try again.", true);
        show_list();
        return;
    }
    s_v.busy = true;
    snprintf(s_v.busy_text, sizeof(s_v.busy_text), "%s", busy_text);
    show_list();
}

static void on_screen_delete(lv_event_t *e) {
    (void)e;
    if (s_v.poll) {
        lv_timer_delete(s_v.poll);
    }
    s_v.screen = NULL;
    s_v.menu   = NULL;
    s_v.poll   = NULL;
    s_v.group  = NULL;
}

static void open_notes(lv_obj_t *screen) {
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    if (!s_v.items) {
        s_v.items = lichen_alloc(&notes_list_app, MAX_NOTES * sizeof(notes_item_t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO); // kept for good
    }
    s_v.screen = screen;
    s_v.menu   = NULL;
    s_v.group  = lv_group_get_default();
    s_v.poll   = lv_timer_create(on_poll, POLL_MS, NULL);
    lv_obj_add_event_cb(screen, on_screen_delete, LV_EVENT_DELETE, NULL);
    s_v.result[0] = s_v.focus_name[0] = '\0';
    s_v.focus_index                   = 0;
    s_v.no_card                       = false;
    if (!s_v.items) {
        s_v.count = 0;
        lv_obj_t *menu = menu_create(screen, "Notes");
        menu_note(menu, "Not enough memory to open the notes. Restart the badge.");
        lv_group_add_obj(s_v.group, screen);
        return;
    }
    if (atomic_load(&s_state) == ST_IDLE) {
        s_req.kind    = JOB_LOAD;
        s_req.name[0] = '\0';
    }
    s_v.busy = true;
    snprintf(s_v.busy_text, sizeof(s_v.busy_text), "Reading notes...");
    if (!job_start() && atomic_load(&s_state) == ST_IDLE) {
        s_v.busy = false; // no task: say so instead of waiting for nothing
        set_result("Not started: out of memory.", true);
    } // else a job from an earlier visit is still running: its result arrives through the poll
    fill_list();
}

// ---- adding and editing -------------------------------------------------------------------------------------------

static void on_new_done(char const *text, void *arg) {
    note_kind_t kind = (note_kind_t)(intptr_t)arg;
    char        body[NOTES_LINE_MAX];
    notes_clean_line(text, strlen(text), body, sizeof(body));
    if (!body[0]) {
        set_result("Not saved: the text was empty.", true);
        show_list();
        return;
    }
    s_req.kind    = JOB_ADD;
    s_req.name[0] = '\0';
    if (!notes_format(kind, body, s_req.line, sizeof(s_req.line))) {
        set_result("Not saved: the text is too long.", true);
        show_list();
        return;
    }
    begin_job("Saving...");
}

static void ask_new(void *arg) {
    note_kind_t kind = (note_kind_t)(intptr_t)arg;
    text_entry_open(kind == NOTE_TODO ? "New to do" : "New note", "", false, NOTES_TEXT_MAX, on_new_done, arg);
}

// ---- one note -----------------------------------------------------------------------------------------------------

static void write_kind(note_kind_t kind, char const *body) {
    notes_item_t const *it = &s_v.items[s_v.shown];
    s_req.kind             = JOB_WRITE;
    snprintf(s_req.name, sizeof(s_req.name), "%s", it->name);
    if (!notes_format(kind, body, s_req.line, sizeof(s_req.line))) {
        set_result("Not saved: the text is too long.", true);
        show_list();
        return;
    }
    begin_job("Saving...");
}

static void on_tick(void *arg) {
    (void)arg;
    char const *body;
    note_kind_t kind = notes_parse(s_v.items[s_v.shown].line, &body);
    char        keep[NOTES_LINE_MAX];
    snprintf(keep, sizeof(keep), "%s", body); // body points into the item the job is about to replace
    write_kind(notes_next_kind(kind), keep);
}

static void on_edit_done(char const *text, void *arg) {
    (void)arg;
    char body[NOTES_LINE_MAX];
    notes_clean_line(text, strlen(text), body, sizeof(body));
    if (!body[0]) {
        set_result("Not saved: the text was empty.", true);
        show_list();
        return;
    }
    char const *old;
    write_kind(notes_parse(s_v.items[s_v.shown].line, &old), body);
}

static void ask_edit(void *arg) {
    (void)arg;
    char const *body;
    notes_parse(s_v.items[s_v.shown].line, &body);
    text_entry_open("Edit note", body, false, NOTES_TEXT_MAX, on_edit_done, NULL);
}

static void on_delete(void *arg) {
    (void)arg;
    s_req.kind = JOB_DELETE;
    snprintf(s_req.name, sizeof(s_req.name), "%s", s_v.items[s_v.shown].name);
    s_v.focus_index = s_v.shown;
    begin_job("Deleting...");
}

static lv_obj_t *build_item(void) {
    if (s_v.shown >= s_v.count) {
        return NULL;
    }
    char const *body;
    note_kind_t kind   = notes_parse(s_v.items[s_v.shown].line, &body);
    lv_obj_t   *screen = shell_page_create();
    lv_obj_t   *menu   = menu_create_named(screen, kind == NOTE_PLAIN ? "Note" : kind == NOTE_TODO ? "To do" : "To do (done)", body);
    if (strlen(body) > LONG_BODY) {
        lv_obj_set_style_text_color(menu_note(menu, body), lv_color_hex(THEME_TEXT), 0);
    }
    menu_add(menu, kind == NOTE_PLAIN ? "Make it a to do" : kind == NOTE_TODO ? "Mark done" : "Mark not done", NULL, on_tick, NULL);
    menu_add(menu, "Edit...", NULL, ask_edit, NULL);
    lv_obj_t *del = menu_add(menu, "Delete", NULL, on_delete, NULL);
    menu_set_kind(del, MENU_DANGER);
    menu_set_danger(del, "Delete this note?", "delete");
    shell_page_hint(screen, "Enter", "choose", NULL);
    return screen;
}

static void on_item(void *arg) {
    s_v.shown = (size_t)(intptr_t)arg;
    shell_open(build_item, SCREEN_NOTE_ITEM);
}

// ---- the app ------------------------------------------------------------------------------------------------------

// Counted when the list was last read; nothing before the first visit (the card is not read for a tile)
static bool notes_tile(char *l1, size_t c1, char *l2, size_t c2) {
    if (!s_counted) {
        return false;
    }
    if (s_open_todos) {
        snprintf(l1, c1, "%u to do", s_open_todos);
    } else {
        snprintf(l1, c1, "No open to dos");
    }
    l2[0] = '\0';
    (void)c2;
    return true;
}

// A sheet with a ticked box and a line
static void icon_pad(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    int line_h = size_px >= 64 ? 4 : 2;
    int box    = size_px / 7;
    for (int i = 0; i < 3; i++) {
        int y = size_px * 9 / 32 + i * (size_px * 3 / 16);
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, box, box);
        lv_obj_set_style_border_width(b, line_h, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(i == 0 ? THEME_TEAL : THEME_AMBER), 0);
        if (i == 0) {
            lv_obj_set_style_bg_color(b, lv_color_hex(THEME_TEAL), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        }
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, size_px / 5, y);
        lv_obj_t *l = lv_obj_create(parent);
        lv_obj_remove_style_all(l);
        lv_obj_set_size(l, size_px * 2 / 5, line_h);
        lv_obj_set_style_bg_color(l, lv_color_hex(THEME_DIM), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, size_px / 5 + box + size_px / 16, y + box / 2 - line_h / 2);
    }
}

static lichen_app_t const notes_list_app = {
    .name      = "Notes",
    .id        = "notes",
    .draw_icon = icon_pad,
    .tile_text = notes_tile,
    .open      = open_notes,
};

void notepad_app_register(void) {
    lichen_register_app(&notes_list_app);
}
