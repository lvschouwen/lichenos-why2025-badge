// Files (#71): browse /storage and the SD card, see free space, read text files a screen at a time, delete (after a
// careful question) and copy between the two volumes. Up/Down select, Enter opens, Backspace deletes, C copies to the other
// volume, Esc goes up a folder (and out of the app at the top). All file work runs on files_job.c's task; this file is
// the screens. The OS's own files (MeshCore identity and contacts, staged C6 images, ...) are listed but locked: the
// rules are files_model.c's and files_ops.c enforces them too.
#include "app_icons.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fonts.h"
#include "files_job.h"
#include "lichen_app.h"
#include "lichen_ui.h"
#include "menu.h"
#include "sd_policy.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "files";

#define SCREEN_FILES_VIEW 180
#define POLL_MS           100
#define ASK_SECONDS       20
#define VIEW_FALLBACK_LINES 12 // if the screen has no size yet when the viewer is built
#define VIEW_PAGES        512 // 512 screens is about 300 KB of text: the viewer is for configs and logs, not for books
#define TEXT_CAP          4096 // one screen of text; a page that would not fit just ends sooner

// The browser; zeroed while closed. LVGL task only.
static struct {
    lv_obj_t          *screen, *menu, *note;
    lv_group_t        *group;
    lv_timer_t        *poll;
    char               dir[FILES_PATH_MAX]; // "" = the volume list
    char               select[FILES_NAME_MAX]; // the row to select after the next listing
    char               message[96];            // the last outcome, shown on top
    char               space[64];              // "1.2 MB free of 4 MB" for dir
    char               target[FILES_PATH_MAX]; // what a delete question is about; after a copy, the copied name
    char               outcome[96];            // the finished delete or copy, shown once the folder is read again
    files_job_result_t root;                   // free space of both volumes, from the last root job
    size_t             count;
    bool               discard; // a job from an earlier visit is still finishing: drop its result, then start
    bool               have_root;
} s_v;

// The viewer's state, ~5 KB: in PSRAM while a viewer is open, NULL otherwise
typedef struct {
    lv_obj_t   *text, *hint;
    lv_timer_t *poll;
    lv_font_t const *font; // the text's font: widths for the word wrap come from it
    int         width_px, lines; // the room between the heading and the page line, measured when the viewer is built
    char      path[FILES_PATH_MAX];
    char      page_text[TEXT_CAP];
    uint32_t  offs[VIEW_PAGES];
    size_t    page, consumed;
    uint32_t  size;
    bool      at_end, binary, loaded;
} view_t;
static view_t *s_t;

static void list_dir(void);
static void on_row(void *arg);

// ---- list ---------------------------------------------------------------------------------------------------------

static void set_note(void) {
    if (s_v.note) {
        lv_label_set_text(s_v.note, s_v.message);
    }
}

static void say(char const *text) {
    snprintf(s_v.message, sizeof(s_v.message), "%s", text);
    set_note();
}

static void size_text(uint64_t bytes, char *out, size_t cap) {
    sd_fmt_size(bytes, out, cap);
}

static void volume_value(int slot, char *out, size_t cap) {
    if (!s_v.root.have[slot]) {
        snprintf(out, cap, "%s", slot ? "no card" : "unavailable");
        return;
    }
    char free_text[16];
    size_text(s_v.root.free_bytes[slot], free_text, sizeof(free_text));
    snprintf(out, cap, "%s free", free_text);
}

static void add_volume_rows(void) {
    if (!s_v.have_root) {
        return; // the first volume job is still running
    }
    static char values[2][32];
    volume_value(0, values[0], sizeof(values[0]));
    volume_value(1, values[1], sizeof(values[1]));
    menu_add(s_v.menu, "Internal storage", values[0], on_row, (void *)(intptr_t)0);
    menu_add(s_v.menu, "SD card", values[1], on_row, (void *)(intptr_t)1);
    shell_page_hint(s_v.screen, "Enter", "open", NULL);
}

static void add_entry_rows(void) {
    files_entry_t const *entries = files_job_entries();
    for (size_t i = 0; i < s_v.count; i++) {
        char label[FILES_NAME_MAX + 2], value[24] = "";
        char path[FILES_PATH_MAX];
        snprintf(label, sizeof(label), "%s%s", entries[i].name, entries[i].dir ? "/" : "");
        if (files_path_join(s_v.dir, entries[i].name, path, sizeof(path)) && files_protected_entry(path, entries[i].dir)) {
            snprintf(value, sizeof(value), "locked");
        } else if (!entries[i].dir) {
            size_text(entries[i].size, value, sizeof(value));
        }
        menu_add(s_v.menu, label, value, on_row, (void *)(intptr_t)i);
    }
    if (!s_v.count) {
        menu_empty(s_v.menu, "files", "copy some here from the other drive");
    }
    shell_page_hint(s_v.screen, "Enter", "open", "Backspace", "delete", "C", "copy", NULL);
}

static bool focused_index(int *out);
static bool on_text(uint32_t key, void *arg);

// shell_open puts the screen into the group when the first build has no rows yet (the volume job is still running); it
// then stayed a stop of its own, and Down past the last row wrapped onto it: no row highlighted, keys dead (#143). With
// rows the screen leaves the group; without rows (an empty folder) it takes the keys, Esc included.
static void keep_only_rows_focusable(void) {
    bool rows = s_v.dir[0] ? s_v.count > 0 : s_v.have_root;
    if (rows && lv_obj_get_group(s_v.screen) == s_v.group) {
        lv_group_remove_obj(s_v.screen);
    } else if (!rows && lv_obj_get_group(s_v.screen) != s_v.group) {
        lv_group_add_obj(s_v.group, s_v.screen);
    }
}

// (Re)builds the menu from s_v.dir and the last listing; rows join the screen's group
static void fill_menu(void) {
    if (s_v.menu) {
        lv_obj_delete(s_v.menu);
    }
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(s_v.group);
    // In a folder the folder names the page (R1): its own name large, where it lives small over it ("Files in
    // /sd/photos"), so a deep path cuts the parents, never the folder you are in (#135 review); the free space as a dim line
    if (s_v.dir[0]) {
        char const *slash = strrchr(s_v.dir, '/');
        char        kind[FILES_PATH_MAX + 16];
        if (slash && slash != s_v.dir) {
            snprintf(kind, sizeof(kind), "Files in %.*s", (int)(slash - s_v.dir), s_v.dir);
        } else {
            snprintf(kind, sizeof(kind), "Files");
        }
        bool nested = slash && slash != s_v.dir; // a volume's own root ("/sd") keeps its full name
        s_v.menu    = menu_create_named(s_v.screen, kind, nested ? slash + 1 : s_v.dir);
    } else {
        s_v.menu = menu_create(s_v.screen, "Files");
    }
    s_v.note = menu_note(s_v.menu, s_v.message);
    lv_obj_set_style_text_color(s_v.note, lv_color_hex(THEME_AMBER), 0);
    if (s_v.dir[0]) {
        menu_note(s_v.menu, s_v.space);
        add_entry_rows();
    } else {
        add_volume_rows();
    }
    menu_on_text(s_v.menu, on_text, NULL);
    lv_group_set_default(previous);
    keep_only_rows_focusable();
    int focus = 0;
    files_entry_t const *entries = files_job_entries();
    for (size_t i = 0; s_v.dir[0] && s_v.select[0] && i < s_v.count; i++) {
        if (!strcmp(entries[i].name, s_v.select)) {
            focus = (int)i;
        }
    }
    if (!s_v.dir[0] && !strcmp(s_v.select, "sd")) {
        focus = 1; // back from the SD card: the volume you came from, not the first
    }
    s_v.select[0] = '\0';
    menu_focus_row(s_v.menu, focus);
}

// The folder (or the volume list) was read: the message line starts over with the outcome of the action before it
static void clear_message(void) {
    snprintf(s_v.message, sizeof(s_v.message), "%s", s_v.outcome);
    s_v.outcome[0] = '\0';
}

static void list_done(files_job_result_t const *r) {
    clear_message();
    if (r->result != FILES_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Can't read this folder (%s). Try again, or check the card.", strerror(r->err));
        say(msg);
        s_v.dir[0] = '\0'; // back to the volume list
        s_v.count  = 0;
        fill_menu();
        return;
    }
    s_v.count = r->count;
    char total[16], free_text[16];
    if (r->have[0]) {
        size_text(r->total_bytes[0], total, sizeof(total));
        size_text(r->free_bytes[0], free_text, sizeof(free_text));
        snprintf(s_v.space, sizeof(s_v.space), "%s free of %s", free_text, total);
    } else {
        snprintf(s_v.space, sizeof(s_v.space), "no space info");
    }
    if (r->skipped && !s_v.message[0]) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%u entries are not shown (too many or names too long).", (unsigned)r->skipped);
        say(msg);
    }
    fill_menu();
}

static void list_dir(void) {
    if (!files_job_start(s_v.dir[0] ? FJ_LIST : FJ_ROOT, s_v.dir, NULL, 0)) {
        say("Busy, try again.");
        return;
    }
    say("Reading...");
}

// ---- the viewer ---------------------------------------------------------------------------------------------------

// Pixel width of one character in the viewer's font, so lines fill the screen whatever the letters (#144)
static int glyph_width(void *ctx, char const *utf8, size_t n) {
    view_t *v = ctx;
    char    one[5];
    memcpy(one, utf8, n < 4 ? n : 4);
    one[n < 4 ? n : 4] = '\0';
    lv_point_t size;
    lv_text_get_size(&size, one, v->font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void show_page(files_job_result_t const *r) {
    if (!s_t || r->result != FILES_OK || !s_t->text) {
        if (s_t && s_t->hint) {
            lv_label_set_text(s_t->hint, files_result_text(r->result));
        }
        return;
    }
    bool eof = r->got < FILES_READ_CHUNK;
    files_text_page(files_job_data(), r->got, eof, s_t->width_px, s_t->lines, s_t->page_text, sizeof(s_t->page_text),
                    &s_t->consumed, &s_t->binary, glyph_width, s_t);
    s_t->size   = r->file_size;
    s_t->at_end = s_t->consumed == 0 || s_t->offs[s_t->page] + s_t->consumed >= r->file_size || s_t->page + 1 >= VIEW_PAGES;
    s_t->loaded = true;
    lv_label_set_text(s_t->text, s_t->page_text[0] ? s_t->page_text : "(empty)");
    char hint[96], size[16];
    size_text(r->file_size, size, sizeof(size));
    snprintf(hint, sizeof(hint), "%sPage %u \xE2\x80\xA2 %s", s_t->binary ? "Binary data. " : "",
             (unsigned)(s_t->page + 1), size);
    lv_label_set_text(s_t->hint, hint);
}

static void read_page(void) {
    if (!files_job_start(FJ_READ, s_t->path, NULL, s_t->offs[s_t->page])) {
        lv_label_set_text(s_t->hint, "Busy, try again.");
    }
}

static void on_view_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (files_job_busy() || !s_t->loaded) {
        return;
    }
    bool next = key == LV_KEY_DOWN || key == LV_KEY_RIGHT;
    bool prev = key == LV_KEY_UP || key == LV_KEY_LEFT;
    if (next && !s_t->at_end) {
        s_t->offs[s_t->page + 1] = s_t->offs[s_t->page] + (uint32_t)s_t->consumed;
        s_t->page++;
    } else if (prev && s_t->page > 0) {
        s_t->page--;
    } else {
        return;
    }
    s_t->loaded = false;
    read_page();
}

static void on_view_delete(lv_event_t *e) {
    (void)e;
    if (s_t) {
        lv_timer_delete(s_t->poll);
        heap_caps_free(s_t);
        s_t = NULL;
    }
}

// The browser collects the page's result while it exists; when the shell closed it under the viewer
// (shell_close_all_but_top) nobody else would, so the viewer takes it itself
static void view_poll(lv_timer_t *timer) {
    (void)timer;
    files_job_result_t r;
    if (!s_v.screen && files_job_take(&r) && r.kind == FJ_READ) {
        show_page(&r);
    }
}

// The page is as many lines as fit between the heading and the "Page N" line, measured from the laid-out screen (its
// bottom padding already holds the pinned key hint), so neither goes off screen whatever the heading's height
static void size_page(lv_obj_t *screen, lv_obj_t *heading) {
    lv_obj_update_layout(screen);
    s_t->font     = lv_obj_get_style_text_font(s_t->text, LV_PART_MAIN);
    s_t->width_px = lv_obj_get_content_width(s_t->text);
    int32_t line  = lv_font_get_line_height(s_t->font) + lv_obj_get_style_text_line_space(s_t->text, LV_PART_MAIN);
    int32_t gap   = lv_obj_get_style_pad_row(screen, LV_PART_MAIN);
    int32_t free_h = lv_obj_get_content_height(screen) - lv_obj_get_height(heading) -
                     lv_font_get_line_height(lv_obj_get_style_text_font(s_t->hint, LV_PART_MAIN)) - 2 * gap;
    s_t->lines    = line > 0 && free_h >= line ? (int)(free_h / line) : VIEW_FALLBACK_LINES;
    if (s_t->width_px < 1) {
        s_t->width_px = 400;
    }
    lv_obj_set_height(s_t->text, s_t->lines * line); // the page line sits right under the last text line
}

static lv_obj_t *build_view(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);
    lv_obj_t *heading = lichen_heading(screen, "File", strrchr(s_t->path, '/') + 1); // the name wraps to two lines (R1, R2)
    s_t->text = lv_label_create(screen);
    lv_obj_set_width(s_t->text, LV_PCT(100));
    lv_label_set_long_mode(s_t->text, LV_LABEL_LONG_WRAP); // a safety net; the page is already cut to this width
    lv_label_set_text(s_t->text, "Reading...");
    s_t->poll = lv_timer_create(view_poll, POLL_MS, NULL);
    s_t->hint = lv_label_create(screen);
    lv_obj_set_width(s_t->hint, LV_PCT(100));
    lv_obj_set_style_text_color(s_t->hint, lv_color_hex(THEME_DIM), 0); // page and size; messages turn it amber
    lv_label_set_text(s_t->hint, "");
    shell_page_hint(screen, "Up/Down", "page", NULL);
    size_page(screen, heading);
    lv_group_add_obj(lv_group_get_default(), screen);
    lv_obj_add_event_cb(screen, on_view_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(screen, on_view_delete, LV_EVENT_DELETE, NULL);
    read_page();
    return screen;
}

static void open_view(char const *path) {
    if (s_t) {
        return;
    }
    s_t = heap_caps_calloc(1, sizeof(*s_t), MALLOC_CAP_SPIRAM);
    if (!s_t) {
        say("Not enough memory to view it.");
        return;
    }
    snprintf(s_t->path, sizeof(s_t->path), "%s", path);
    shell_open(build_view, SCREEN_FILES_VIEW);
    if (s_t && !s_t->text) { // the shell did not open it (no builder ran): nothing will free the state
        heap_caps_free(s_t);
        s_t = NULL;
    }
}

// ---- actions ------------------------------------------------------------------------------------------------------

static bool entry_path(size_t index, char *out, size_t cap) {
    files_entry_t const *entries = files_job_entries();
    return index < s_v.count && files_path_join(s_v.dir, entries[index].name, out, cap);
}

static void on_row(void *arg) {
    size_t index = (size_t)(intptr_t)arg;
    if (files_job_busy()) {
        return;
    }
    if (!s_v.dir[0]) {
        if (index && !s_v.root.have[1]) { // no card: say what to do, instead of a failed read
            say("No SD card. Insert one, then see Settings > SD card.");
            return;
        }
        snprintf(s_v.dir, sizeof(s_v.dir), "%s", index ? FILES_SD : FILES_STORAGE);
        list_dir();
        return;
    }
    char path[FILES_PATH_MAX];
    if (!entry_path(index, path, sizeof(path))) {
        return;
    }
    if (files_job_entries()[index].dir) {
        snprintf(s_v.dir, sizeof(s_v.dir), "%s", path);
        list_dir();
    } else if (files_secret(path)) {
        say("This file holds secrets, so it is not shown.");
    } else {
        open_view(path);
    }
}

static void delete_answer(bool yes) {
    if (yes && s_v.screen && files_job_start(FJ_DELETE, s_v.target, NULL, 0)) {
        say("Deleting...");
    }
}

static void ask_delete(size_t index) {
    static char title[FILES_NAME_MAX + 48];
    if (!entry_path(index, s_v.target, sizeof(s_v.target))) {
        return;
    }
    if (files_protected_entry(s_v.target, files_job_entries()[index].dir)) {
        say(files_result_text(FILES_ERR_PROTECTED));
        return;
    }
    snprintf(title, sizeof(title), "Delete %s%s?", files_job_entries()[index].name,
             files_job_entries()[index].dir ? " (folder)" : "");
    shell_dialog_t const d = {
        .title         = title,
        .seconds       = ASK_SECONDS,
        .careful       = true,
        .confirm_label = "delete",
    };
    shell_dialog_countdown(&d, delete_answer);
}

static void start_copy(size_t index) {
    char src[FILES_PATH_MAX], dst[FILES_PATH_MAX];
    files_entry_t const *e = &files_job_entries()[index];
    if (!entry_path(index, src, sizeof(src))) {
        return;
    }
    if (e->dir) {
        say(files_result_text(FILES_ERR_NOT_FILE));
    } else if (files_secret(src)) {
        say("This file holds secrets, so it is not copied.");
    } else if (!files_path_join(files_other_volume(files_volume_of(src)), e->name, dst, sizeof(dst))) {
        say(files_result_text(FILES_ERR_ARG));
    } else if (files_job_start(FJ_COPY, src, dst, 0)) {
        snprintf(s_v.target, sizeof(s_v.target), "%s", e->name);
        say("Copying...");
    }
}

static bool focused_index(int *out) {
    lv_obj_t *focused = lv_group_get_focused(s_v.group);
    int       idx     = 0;
    for (uint32_t i = 0; s_v.menu && focused && i < lv_obj_get_child_count(s_v.menu); i++) {
        lv_obj_t *child = lv_obj_get_child(s_v.menu, (int32_t)i);
        if (!lv_obj_get_user_data(child)) {
            continue;
        }
        if (child == focused) {
            *out = idx;
            return true;
        }
        idx++;
    }
    return false;
}

static bool on_text(uint32_t key, void *arg) {
    (void)arg;
    int index;
    if (!s_v.dir[0] || files_job_busy() || !focused_index(&index)) {
        return false;
    }
    if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
        ask_delete((size_t)index);
    } else if (key == 'c' || key == 'C') {
        start_copy((size_t)index);
    }
    return true;
}

// Esc: stops a copy, goes up a folder, leaves from the volume list
static bool on_esc(void *arg) {
    (void)arg;
    if (files_job_busy()) {
        files_job_cancel();
        return true;
    }
    if (!s_v.dir[0]) {
        return false;
    }
    char parent[FILES_PATH_MAX];
    snprintf(s_v.select, sizeof(s_v.select), "%s", strrchr(s_v.dir, '/') + 1);
    if (files_path_parent(s_v.dir, parent, sizeof(parent))) {
        snprintf(s_v.dir, sizeof(s_v.dir), "%s", parent);
    }
    list_dir();
    return true;
}

// ---- the job's answers --------------------------------------------------------------------------------------------

static void action_done(files_job_result_t const *r) {
    if (r->kind == FJ_COPY) {
        char msg[96];
        snprintf(msg, sizeof(msg), r->result == FILES_OK ? "Copied %s." : "%s", r->result == FILES_OK ? s_v.target : files_result_text(r->result));
        say(msg);
    } else {
        say(r->result == FILES_OK ? "Deleted." : files_result_text(r->result));
    }
    ESP_LOGI(TAG, "%s: %s", r->kind == FJ_COPY ? "copy" : "delete", files_result_text(r->result));
    snprintf(s_v.outcome, sizeof(s_v.outcome), "%s", s_v.message);
    list_dir();
}

static void progress_tick(void) {
    uint32_t done, total;
    files_job_progress(&done, &total);
    if (s_v.note && total && !strncmp(lv_label_get_text(s_v.note), "Copying", 7)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Copying %u%%. Esc: cancel", (unsigned)((uint64_t)done * 100 / total));
        say(msg);
    }
}

static void on_poll(lv_timer_t *timer) {
    (void)timer;
    files_job_result_t r;
    if (!files_job_take(&r)) {
        progress_tick();
        return;
    }
    if (s_v.discard) {
        s_v.discard = false;
        list_dir();
        return;
    }
    switch (r.kind) {
    case FJ_ROOT:
        s_v.root      = r;
        s_v.have_root = true;
        s_v.count     = 0;
        clear_message();
        fill_menu();
        break;
    case FJ_LIST:
        list_done(&r);
        break;
    case FJ_READ:
        show_page(&r);
        break;
    default:
        action_done(&r);
        break;
    }
}

// ---- the app ------------------------------------------------------------------------------------------------------

static void on_screen_delete(lv_event_t *e) {
    (void)e;
    lv_timer_delete(s_v.poll);
    files_job_cancel(); // a running copy removes its partial file and stops
    memset(&s_v, 0, sizeof(s_v));
}

static void open_files(lv_obj_t *screen) {
    memset(&s_v, 0, sizeof(s_v));
    s_v.screen = screen;
    s_v.group  = lv_group_get_default(); // shell_open's group for this screen while it is built
    s_v.poll   = lv_timer_create(on_poll, POLL_MS, NULL);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    shell_on_esc(on_esc, NULL);
    lv_obj_add_event_cb(screen, on_screen_delete, LV_EVENT_DELETE, NULL);
    files_job_result_t stale;
    if (files_job_busy()) {
        s_v.discard = true; // an earlier visit's job: wait for it, then start
        fill_menu();
        return;
    }
    files_job_take(&stale); // a finished job nobody collected
    fill_menu();
    list_dir();
}

static lichen_app_t const files_app = {
    .name      = "Files",
    .id        = "files",
    .draw_icon = icon_files,
    .open      = open_files,
};

void files_app_register(void) {
    lichen_register_app(&files_app);
}
