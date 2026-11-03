// Release Notes (#13): the notes in firmware/release_notes/ are compiled into the image (rodata, read through plain
// pointers: no flash mapping, #31), so they always match the running build. This file is the app on home (a list, one
// full page per note, a badge for the unread ones) and the "This is new" screen that follows an update.
// The marker is the highest note sequence the person has seen (NVS notes_seen); release_notes.h has the rules.
#include "app_icons.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "fonts.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "lichen_badge.h"
#include "lichen_ui.h"
#include "menu.h"
#include "nav.h"
#include "notes_app.h"
#include "release_notes.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "theme.h"
#include "update_screen.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "notes";

#define SCREEN_NOTES_ENTRY 190
#define SCREEN_NOTES_NEW   191
#define SEEN_KEY           "notes_seen"
#define SCROLL_STEP        96
#define WAIT_TICK_MS       250
#define TITLE_CAP          48

static lichen_app_t const notes_app;

static rn_note_t *s_notes; // newest first; PSRAM, allocated once by load() (internal RAM is scarce)
static size_t    s_count;
static bool      s_loaded;
static bool      s_seen_known;
static uint32_t  s_seen;   // the marker; LVGL task and console (rare), a word each
static size_t    s_shown;  // the note on the entry page
static uint32_t  s_seen_at_open; // the marker when the list opened: rows above it say "new"

static void load(void) {
    if (s_loaded) {
        return;
    }
    // s_loaded last: until then another caller would read a half-built index (#118)
    s_notes = heap_caps_calloc(RN_MAX_NOTES, sizeof(rn_note_t), MALLOC_CAP_SPIRAM);
    if (!s_notes) {
        ESP_LOGW(TAG, "no memory for the release notes index");
        s_loaded = true;
        return; // s_count stays 0: the app says there are no notes
    }
    s_count = rn_build_index(rn_files, rn_file_count, s_notes, RN_MAX_NOTES);
    char value[16];
    settings_get_str(SEEN_KEY, value, sizeof(value), "");
    s_seen   = rn_parse_seen(value, &s_seen_known);
    s_loaded = true;
}

static uint32_t newest(void) {
    return s_count ? s_notes[0].seq : 0;
}

static void update_badge(void) {
    lichen_badge_set(notes_app.id, (unsigned)rn_count_new(s_notes, s_count, s_seen_known, s_seen));
}

// Everything up to the newest note counts as seen
static void mark_seen(void) {
    if (!s_count) {
        return;
    }
    char value[16];
    snprintf(value, sizeof(value), "%u", (unsigned)newest());
    if (!settings_set_str(SEEN_KEY, value)) {
        ESP_LOGW(TAG, "could not save the notes marker");
    }
    s_seen       = newest();
    s_seen_known = true;
    update_badge();
}

// ---- pages ----

static lv_obj_t *page_create(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 16, 0);
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);
    shell_scroll_tame(screen);
    return screen;
}

static lv_obj_t *add_text(lv_obj_t *parent, char const *text, uint32_t color, lv_font_t const *font) {
    lv_obj_t *label = lichen_label_wrap(parent, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_label_set_text(label, text);
    return label;
}

static void on_page_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_target(e), key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
    }
}

static void on_new_key(lv_event_t *e) {
    if (lv_event_get_key(e) == LV_KEY_ENTER) {
        shell_back();
    } else {
        on_page_key(e);
    }
}

static lv_obj_t *build_entry(void) {
    if (s_shown >= s_count) {
        return NULL;
    }
    lv_obj_t *screen = page_create();
    notes_render(screen, s_notes[s_shown].text, s_notes[s_shown].date);
    shell_page_hint(screen, "Up/Down", "scroll", NULL);
    lv_obj_add_event_cb(screen, on_page_key, LV_EVENT_KEY, NULL);
    return screen;
}

// "This is new": every note above the marker, newest first. The marker moves as the screen is built, so a note shows
// once even if the badge restarts while it is read.
static lv_obj_t *build_new(void) {
    size_t fresh = rn_count_new(s_notes, s_count, s_seen_known, s_seen);
    if (!fresh) {
        return NULL;
    }
    lv_obj_t *screen = page_create();
    add_text(screen, "This is new", THEME_AMBER, lichen_font(LICHEN_FONT_LARGE));
    for (size_t i = 0; i < fresh; i++) {
        notes_render(screen, s_notes[i].text, s_notes[i].date);
    }
    shell_page_hint(screen, "Up/Down", "scroll", "Enter", "continue", NULL);
    lv_obj_add_event_cb(screen, on_new_key, LV_EVENT_KEY, NULL);
    mark_seen();
    return screen;
}

// ---- the app ----

static void on_note(void *arg) {
    s_shown = (size_t)(intptr_t)arg;
    shell_open_keyed(build_entry, SCREEN_NOTES_ENTRY, (uint32_t)(intptr_t)arg + 1); // one screen per note
}

typedef struct {
    char title[TITLE_CAP];
    char value[RN_DATE_LEN + 8];
} row_text_t;

static void on_list_delete(lv_event_t *e) {
    heap_caps_free(lv_event_get_user_data(e));
}

static void open_notes(lv_obj_t *screen) {
    load();
    s_seen_at_open = s_seen_known ? s_seen : 0;
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *menu = menu_create(screen, "Release notes");
    if (!s_count) {
        menu_empty(menu, "release notes", NULL);
        return;
    }
    // Row texts in PSRAM for as long as the list is open
    row_text_t *rows = heap_caps_calloc(s_count, sizeof(row_text_t), MALLOC_CAP_SPIRAM);
    if (!rows) {
        menu_note(menu, "Not enough memory to list the notes.");
        return;
    }
    lv_obj_add_event_cb(screen, on_list_delete, LV_EVENT_DELETE, rows);
    for (size_t i = 0; i < s_count; i++) {
        rn_title(s_notes[i].text, rows[i].title, sizeof(rows[i].title));
        bool fresh = s_notes[i].seq > s_seen_at_open;
        snprintf(rows[i].value, sizeof(rows[i].value), "%s%s", fresh ? "new  " : "", s_notes[i].date);
        menu_add(menu, rows[i].title, rows[i].value, on_note, (void *)(intptr_t)i);
    }
    shell_page_hint(screen, "Enter", "open", NULL);
    mark_seen(); // looking at the list counts as seen; the rows above still say "new"
}

static bool notes_tile(char *l1, size_t c1, char *l2, size_t c2) {
    load();
    size_t fresh = rn_count_new(s_notes, s_count, s_seen_known, s_seen);
    // Nothing new says so: "false" would keep "N new" on the tile after the notes were read (#128)
    if (fresh) {
        snprintf(l1, c1, "%u new", (unsigned)fresh);
    } else {
        snprintf(l1, c1, "Up to date");
    }
    if (s_count) {
        rn_title(s_notes[0].text, l2, c2);
    } else {
        l2[0] = '\0';
    }
    return true;
}

static lichen_app_t const notes_app = {
    .name      = "Release notes",
    .id        = "relnotes",
    .draw_icon = icon_notes,
    .tile_text = notes_tile,
    .open      = open_notes,
};

// ---- after boot ----

static void wait_tick(lv_timer_t *timer) {
    // Home is up and its fade is over, the update banner is gone, no setup question or update is in the way
    if (!shell_home_shown() || update_screen_banner_active() || setup_running() || update_screen_busy() ||
        nav_depth() != 1) {
        return;
    }
    lv_timer_delete(timer);
    shell_open(build_new, SCREEN_NOTES_NEW);
}

// Main task, while the LVGL task already renders (and reads the notes for home's tile and the pages): the notes state is
// LVGL-task state, so the whole check holds the recursive LVGL lock (#118)
void notes_boot_check(bool updated) {
    lvgl_port_lock(0);
    load();
    rn_boot_t what = rn_boot_decision(updated, s_seen_known, s_seen, newest());
    ESP_LOGI(TAG, "%u notes, marker %s%u, %s: %s", (unsigned)s_count, s_seen_known ? "" : "unset ", (unsigned)s_seen,
             updated ? "after an update" : "normal boot",
             what == RN_BOOT_SHOW ? "showing" : what == RN_BOOT_MARK ? "marking seen" : "nothing to show");
    if (what == RN_BOOT_MARK) {
        mark_seen();
    } else {
        update_badge();
    }
    if (what == RN_BOOT_SHOW) {
        lv_timer_create(wait_tick, WAIT_TICK_MS, NULL);
    }
    lvgl_port_unlock();
}

// fw whatsnew [show|reset]: for the bench. Console task: the notes state is read and changed under the (recursive) LVGL
// lock, like notes_boot_check (#118); the replies go out after it
static void cmd_whatsnew(char const *args) {
    bool reset = strcmp(args, "reset") == 0;
    bool show  = strcmp(args, "show") == 0;
    lvgl_port_lock(0);
    load();
    if (reset) {
        settings_erase(SEEN_KEY);
        s_seen_known = false;
        s_seen       = 0;
        update_badge();
    } else if (show) {
        s_seen_known = true;
        s_seen       = s_count > 1 ? s_notes[1].seq : 0; // the newest note counts as new
        shell_open(build_new, SCREEN_NOTES_NEW);
    }
    unsigned count  = (unsigned)s_count;
    bool     known  = s_seen_known;
    unsigned seen   = (unsigned)s_seen;
    unsigned unread = (unsigned)rn_count_new(s_notes, s_count, s_seen_known, s_seen);
    lvgl_port_unlock();
    if (reset) {
        console_reply("whatsnew: marker erased, %u notes unread", count);
    } else if (show) {
        console_reply("whatsnew: showing the newest note");
    } else {
        console_reply("whatsnew: %u notes, marker %s%u, unread %u (whatsnew show|reset)", count, known ? "" : "unset ",
                      seen, unread);
    }
}

void notes_app_register(void) {
    load(); // before any other task asks (shell_start)
    lichen_register_app(&notes_app);
    console_register("whatsnew", "whatsnew [show|reset] (release notes marker, #13)", cmd_whatsnew);
}
