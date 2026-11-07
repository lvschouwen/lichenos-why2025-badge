// The notification centre (#62): the Notifications app (its home tile until #79's cloud-key shortcut can replace it),
// the list newest first, one notice on its own page, and Settings › Notifications (a switch per source, do-not-disturb,
// Clear all). Opening the centre marks info read; warnings and errors only when opened or cleared (Lucas, #62 Q3).
// Opening a notice goes to its source: the app's open_notification, or the app itself; Lichen's own notices to the
// place they are about (the Crash log, Settings › SD card), else their page here.
#include "app_icons.h"
#include "screen_ids.h"
#include "esp_log.h"
#include "fonts.h"
#include "led.h"
#include "lichen_haptics.h"
#include "lichen_app.h"
#include "lichen_ui.h"
#include "menu.h"
#include "nav.h"
#include "notify_os.h"
#include "notify_view.h"
#include "scroll_anchor.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define POLL_MS                1000
#define ASK_SECONDS            20
#define SCROLL_STEP            96
#define BULLET                 "\xE2\x97\x8F " // U+25CF

static lichen_app_t const notify_app;

// The open centre; all NULL/0 while it is closed. LVGL task only.
static struct {
    lv_obj_t   *screen;
    lv_group_t *group;
    lv_obj_t   *menu;
    lv_obj_t   *rows[NR_SLOTS];
    uint32_t    ids[NR_SLOTS];
    lv_obj_t   *clear_row; // "Clear all"
    size_t      count;
    size_t      focus;        // the selection, kept across rebuilds by id (#128): its row, the notice
    uint32_t    focus_id;     //   on it (0: none yet)
    bool        on_clear_all; //   or "Clear all"
    uint32_t    gen;
    uint32_t    minute; // ages are redrawn when the uptime minute changes
    lv_timer_t *poll;
} s_c;

static nr_entry_t s_entry;       // scratch copy, LVGL task only (too big for its stack many times over)
static uint32_t   s_detail_id;   // the notice on the detail page
static uint32_t   s_ask_id;      // the notice a "clear this unread error?" question is about

static int app_index(char const *id) {
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (app->id && strcmp(app->id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t uptime_s(void) {
    return lv_tick_get() / 1000; // the same clock as esp_timer within a second; only used for ages
}

// The store's boot counter, read once per page or list build
static uint16_t store_boot(void) {
    ln_stats_t st;
    lichen_notify_stats(&st);
    return st.boot;
}

static void age_text(nr_entry_t const *e, uint16_t boot, char *out, size_t cap) {
    time_t now = time(NULL);
    nview_age(out, cap, e->boot, e->mono_s, e->epoch, boot, (uint32_t)(esp_log_timestamp() / 1000),
              e->epoch ? (int64_t)now : 0);
}

// ---- one notice on its own page --------------------------------------------------------------------------------------

// Lichen's own notices name the console command with the details
static char const *console_hint(char const *target) {
    static char const *const map[][2] = {
        {"crash", "fw crash"}, {"clock", "fw clock"}, {"psram", "fw psram"}, {"c6", "fw c6"},
        {"sd", "fw sd"}, {"battery", "fw thermal"},       {"sensors", "fw sensors"}, {"update", "fw version"}, {"storage", "fw storage"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i][0], target) == 0) {
            return map[i][1];
        }
    }
    return NULL;
}

static lv_obj_t *add_text(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lichen_label_wrap(parent, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static void on_detail_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_target(e), key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
    }
}

static lv_obj_t *build_detail(void) {
    if (!lichen_notify_get(s_detail_id, &s_entry)) {
        return NULL;
    }
    lv_obj_t *screen = shell_page_create();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 16, 0);
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);
    char title[NR_TITLE_CAP + 16];
    nview_title(title, sizeof(title), s_entry.title, s_entry.count);
    lv_obj_t *t = add_text(screen, title, notify_level_color(s_entry.level) == THEME_DIM ? THEME_TEXT
                                                                                         : notify_level_color(s_entry.level));
    lv_obj_set_style_text_font(t, lichen_font(LICHEN_FONT_LARGE), 0);
    add_text(screen, s_entry.body, THEME_TEXT);
    char line[96], age[24], when[24] = "";
    age_text(&s_entry, store_boot(), age, sizeof(age));
    struct tm local;
    time_t    at = (time_t)s_entry.epoch;
    if (s_entry.epoch && localtime_r(&at, &local)) {
        strftime(when, sizeof(when), " (%d %b %H:%M)", &local);
    }
    snprintf(line, sizeof(line), "From %s%s%s, %s%s", strcmp(s_entry.source, "lichen") == 0 ? "Lichen" : s_entry.source,
             s_entry.target[0] ? " \xC2\xB7 " : "", s_entry.target, age, when);
    add_text(screen, line, THEME_DIM);
    char const *hint = strcmp(s_entry.source, "lichen") == 0 ? console_hint(s_entry.target) : NULL;
    if (hint) {
        snprintf(line, sizeof(line), "More on the console: %s", hint);
        add_text(screen, line, THEME_DIM);
    }
    shell_page_hint(screen, "Up/Down", "scroll", NULL);
    lv_obj_add_event_cb(screen, on_detail_key, LV_EVENT_KEY, NULL);
    return screen;
}

static void open_detail(uint32_t id) {
    s_detail_id = id;
    shell_open_keyed(build_detail, SCREEN_NOTIFY_DETAIL, id); // one page per notice
}

// ---- opening a notice --------------------------------------------------------------------------------------------------

// Lichen's notices that are about a place of their own (#62 updates: the crash notice opens the Crash log, #72)
static bool open_system(char const *target) {
    if (strcmp(target, "crash") == 0) {
        int i = app_index("crashlog");
        if (i >= 0) {
            shell_open_app((size_t)i);
            return true;
        }
    } else if (strcmp(target, "sd") == 0) {
        sd_screen_open();
        return true;
    } else if (strcmp(target, "battery") == 0) {
        battery_open(NULL);
        return true;
    }
    return false;
}

// Opens where the notice in s_entry belongs
static void open_where(uint32_t id) {
    if (strcmp(s_entry.source, "lichen") == 0) {
        if (!open_system(s_entry.target)) {
            open_detail(id);
        }
        return;
    }
    int                 i   = app_index(s_entry.source);
    lichen_app_t const *app = i >= 0 ? lichen_app_at((size_t)i) : NULL;
    if (!app || strcmp(s_entry.source, NOTIFY_APP_ID) == 0 || (!app->open && !app->open_notification)) {
        open_detail(id); // no such app ("fw notify test"), or one without a screen: its page here
    } else if (app->open_notification) {
        char target[NR_TARGET_CAP];
        snprintf(target, sizeof(target), "%s", s_entry.target);
        app->open_notification(target, id);
    } else {
        shell_open_app((size_t)i);
    }
}

// Marked read only once a screen for it really opened (shell_open refuses while booting or with the stack full)
void notify_open_entry(uint32_t id) {
    if (!lichen_notify_get(id, &s_entry)) {
        return;
    }
    int      depth  = nav_depth();
    unsigned reused = shell_open_reuse_count();
    open_where(id);
    if (nav_depth() > depth || shell_open_reuse_count() != reused) { // a new screen, or the one already open
        lichen_notify_mark_read_id(id);
    }
}

void notify_open_centre(void) {
    int i = app_index(NOTIFY_APP_ID);
    if (i >= 0) {
        shell_open_app((size_t)i);
    }
}

// ---- the list ----------------------------------------------------------------------------------------------------------

static void fill_list(void);

// Only while the centre is on top and no dialog is open (on_poll's rule); otherwise on_poll rebuilds it once it is
static void refill(void *arg) {
    (void)arg;
    if (s_c.screen && lv_screen_active() == s_c.screen && !shell_dialog_group()) {
        fill_list();
    }
}

static void on_row(void *arg) {
    size_t i = (size_t)(uintptr_t)arg;
    if (i < s_c.count) {
        s_c.focus    = i;
        s_c.focus_id = s_c.ids[i];
        notify_open_entry(s_c.ids[i]);
    }
}

static void clear_all_answer(bool yes) {
    if (yes) {
        lichen_notify_clear_all();
        s_c.focus        = 0;
        s_c.focus_id     = 0;
        s_c.on_clear_all = false;
        if (s_c.screen) {
            lv_async_call(refill, NULL);
        }
    }
}

static void ask_clear_all(void) {
    static char title[80];
    snprintf(title, sizeof(title), "Clear all %u notifications?", (unsigned)(s_c.screen ? s_c.count : 0));
    shell_dialog_t const d = {
        .title         = s_c.screen ? title : "Clear all notifications?",
        .seconds       = ASK_SECONDS,
        .careful       = true, // the countdown cancels; Enter counts only after a second
        .confirm_label = "clear all",
    };
    shell_dialog_countdown(&d, clear_all_answer);
}

static void on_clear_all(void *arg) {
    (void)arg;
    ask_clear_all();
}

static void clear_one_answer(bool yes) {
    if (yes && lichen_notify_remove(s_ask_id) && s_c.screen) {
        lv_async_call(refill, NULL);
    }
}

// Backspace or Del on a row clears it; an unread error asks first
static bool on_text(uint32_t key, void *arg) {
    (void)arg;
    if (key != LV_KEY_BACKSPACE && key != LV_KEY_DEL) {
        return false;
    }
    lv_obj_t *focused = lv_group_get_focused(s_c.group);
    for (size_t i = 0; i < s_c.count; i++) {
        if (s_c.rows[i] != focused || !lichen_notify_get(s_c.ids[i], &s_entry)) {
            continue;
        }
        if (s_entry.level == LN_ERROR && !s_entry.read) {
            s_ask_id             = s_c.ids[i];
            shell_dialog_t const d = {
                .title         = "Clear this unread error?",
                .seconds       = ASK_SECONDS,
                .careful       = true,
                .confirm_label = "clear",
            };
            shell_dialog_countdown(&d, clear_one_answer);
        } else if (lichen_notify_remove(s_c.ids[i])) {
            lv_async_call(refill, NULL); // not from inside the row's own key handling
        }
        return true;
    }
    return false;
}

static void add_rows(void) {
    s_c.count      = lichen_notify_ids(s_c.ids, NR_SLOTS);
    size_t   shown = 0;
    uint16_t boot  = store_boot();
    for (size_t i = 0; i < s_c.count; i++) {
        if (!lichen_notify_get(s_c.ids[i], &s_entry)) {
            continue;
        }
        char title[NR_TITLE_CAP + 24], age[24];
        nview_title(title + strlen(BULLET), sizeof(title) - strlen(BULLET), s_entry.title, s_entry.count);
        memcpy(title, BULLET, strlen(BULLET));
        age_text(&s_entry, boot, age, sizeof(age));
        lv_obj_t *row = menu_add(s_c.menu, s_entry.read ? title + strlen(BULLET) : title, age, on_row,
                                 (void *)(uintptr_t)shown);
        uint32_t color = s_entry.read ? THEME_DIM : s_entry.level == LN_INFO ? THEME_TEXT : notify_level_color(s_entry.level);
        lv_obj_set_style_text_color(lv_obj_get_child(row, 0), lv_color_hex(color), 0);
        s_c.ids[shown]    = s_c.ids[i];
        s_c.rows[shown++] = row;
    }
    s_c.count = shown;
    s_c.clear_row = NULL;
    lv_obj_t *hint = shell_page_hint(s_c.screen, "Enter", "open", "Backspace", "clear", NULL);
    if (shown) {
        s_c.clear_row = menu_add(s_c.menu, "Clear all", NULL, on_clear_all, NULL);
        lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        menu_empty(s_c.menu, "notifications", "apps and Lichen post here when they have news");
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN); // nothing to open or clear
    }
    if (lichen_notify_dnd()) {
        menu_note(s_c.menu, "Do not disturb is on: only errors pop up (Settings \xE2\x80\xBA Notifications).");
    }
}

// Notes which row has the selection (by the notice on it), so a rebuild puts it back there
static void note_focus(void) {
    lv_obj_t *focused = s_c.group ? lv_group_get_focused(s_c.group) : NULL;
    if (focused && focused == s_c.clear_row) {
        s_c.on_clear_all = true;
        return;
    }
    for (size_t i = 0; i < s_c.count; i++) {
        if (s_c.rows[i] == focused) {
            s_c.focus        = i;
            s_c.focus_id     = s_c.ids[i];
            s_c.on_clear_all = false;
        }
    }
}

// The notice rows' places in the menu's content, keyed by notice id, for scroll_anchor
static size_t row_items(scroll_item_t *items) {
    for (size_t i = 0; i < s_c.count; i++) {
        items[i].top    = lv_obj_get_y(s_c.rows[i]); // in the content: does not move with scrolling
        items[i].bottom = items[i].top + lv_obj_get_height(s_c.rows[i]);
        items[i].key    = s_c.ids[i];
    }
    return s_c.count;
}

// lichen_ui_with_group's builder: the menu and its rows, in the screen's group
static void build_menu(void *arg) {
    (void)arg;
    s_c.menu = menu_create(s_c.screen, "Notifications");
    menu_on_text(s_c.menu, on_text, NULL);
    add_rows();
}

// (Re)builds the menu; rows join the screen's own group, whatever group is the default now (crash log's pattern). The
// selection stays on its notice and the view where it was: a new notice or a cleared one no longer moves either (#128)
static void fill_list(void) {
    if (!s_c.screen) {
        return;
    }
    static scroll_item_t items[NR_SLOTS]; // LVGL task only
    int32_t              scroll_y = 0;
    scroll_anchor_t      anchor   = {0};
    if (s_c.menu) {
        note_focus();
        scroll_y = lv_obj_get_scroll_y(s_c.menu);
        anchor   = scroll_anchor_take(items, row_items(items), scroll_y);
        lv_obj_delete(s_c.menu);
    }
    lichen_ui_with_group(s_c.group, build_menu, NULL);
    // With rows the screen must not be a stop of its own; without rows it takes the keys (Esc)
    bool rows = s_c.count > 0;
    if (!rows && lv_obj_get_group(s_c.screen) != s_c.group) {
        lv_group_add_obj(s_c.group, s_c.screen);
    } else if (rows && lv_obj_get_group(s_c.screen) == s_c.group) {
        lv_group_remove_obj(s_c.screen);
    }
    if (rows) {
        lv_obj_update_layout(s_c.menu);
        if (scroll_y > 0) {
            scroll_anchor_find(&anchor, items, row_items(items), &scroll_y);
            lv_obj_scroll_to_y(s_c.menu, scroll_y, LV_ANIM_OFF); // bounded: a shorter list is not overscrolled
        }
        size_t at = nview_focus_after(s_c.ids, s_c.count, s_c.focus_id, s_c.focus, s_c.on_clear_all);
        menu_focus_row(s_c.menu, (int)at); // rows count from 0, "Clear all" is row count
        note_focus();
    } else {
        s_c.focus        = 0;
        s_c.focus_id     = 0;
        s_c.on_clear_all = false;
    }
    s_c.gen    = lichen_notify_generation();
    s_c.minute = uptime_s() / 60;
}

// A new minute only changes the ages: redrawn in place, the selection and scroll untouched (#128)
static void update_ages(void) {
    uint16_t boot = store_boot();
    for (size_t i = 0; i < s_c.count; i++) {
        if (lichen_notify_get(s_c.ids[i], &s_entry)) {
            char age[24];
            age_text(&s_entry, boot, age, sizeof(age));
            menu_set_value(s_c.rows[i], age);
        }
    }
    s_c.minute = uptime_s() / 60;
}

static void on_poll(lv_timer_t *timer) {
    (void)timer;
    bool changed = lichen_notify_generation() != s_c.gen;
    if (!changed && uptime_s() / 60 == s_c.minute) {
        return;
    }
    if (!changed) {
        update_ages(); // labels only: fine under a dialog or another screen too
        return;
    }
    // Only while the centre is on top: a rebuild under another screen would move that screen's focus group
    if (lv_screen_active() == s_c.screen && !shell_dialog_group()) {
        fill_list();
    }
}

static void on_screen_delete(lv_event_t *e) {
    (void)e;
    lv_timer_delete(s_c.poll);
    memset(&s_c, 0, sizeof(s_c));
}

static void open_centre(lv_obj_t *screen) {
    memset(&s_c, 0, sizeof(s_c));
    s_c.screen = screen;
    s_c.group  = lv_group_get_default(); // shell_open's group for this screen while it is built
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    fill_list(); // unread info still shows as unread this once
    lichen_notify_mark_read_upto(LN_INFO);
    s_c.gen  = lichen_notify_generation(); // our own mark is no reason to rebuild
    s_c.poll = lv_timer_create(on_poll, POLL_MS, NULL);
    lv_obj_add_event_cb(screen, on_screen_delete, LV_EVENT_DELETE, NULL);
}

static bool tile_text(char *l1, size_t c1, char *l2, size_t c2) {
    size_t unread = lichen_notify_unread(NULL);
    if (!unread) {
        snprintf(l1, c1, "Nothing new");
        l2[0] = '\0';
        return true;
    }
    snprintf(l1, c1, "%u unread", (unsigned)unread);
    uint32_t ids[NR_SLOTS];
    size_t   n = lichen_notify_ids(ids, NR_SLOTS);
    l2[0]      = '\0';
    for (size_t i = 0; i < n; i++) {
        if (lichen_notify_get(ids[i], &s_entry) && !s_entry.read) {
            nview_title(l2, c2, s_entry.title, s_entry.count);
            break;
        }
    }
    return true;
}

static lichen_app_t const notify_app = {
    .name      = "Notifications",
    .id        = NOTIFY_APP_ID,
    .draw_icon = icon_notify,
    .tile_text = tile_text,
    .open      = open_centre,
};

void notify_app_register(void) {
    lichen_register_app(&notify_app);
}

// ---- Settings › Notifications --------------------------------------------------------------------------------------------

#define MAX_SOURCES (LICHEN_MAX_APPS + 1)

static struct {
    lv_obj_t   *dnd_row;
    lv_obj_t   *led_row;
    lv_obj_t   *vib_row;
    lv_obj_t   *vib_str_row;
    lv_obj_t   *clear_row;
    lv_obj_t   *source_row[MAX_SOURCES];
    char const *source[MAX_SOURCES];
} s_s;

static void toggle_source(void *arg) {
    size_t i = (size_t)(uintptr_t)arg;
    bool   on = !lichen_notify_source_enabled(s_s.source[i]);
    lichen_notify_set_source_enabled(s_s.source[i], on);
    menu_set_on(s_s.source_row[i], lichen_notify_source_enabled(s_s.source[i]));
}

static void open_app_notify_settings(void *arg) {
    lichen_app_t const *app = arg;
    app->notify_settings_open();
}

static void toggle_dnd(void *arg) {
    (void)arg;
    lichen_notify_set_dnd(!lichen_notify_dnd());
    menu_set_on(s_s.dnd_row, lichen_notify_dnd());
}

static void toggle_led(void *arg) {
    (void)arg;
    led_set_enabled(!led_enabled());
    menu_set_on(s_s.led_row, led_enabled());
}

static void toggle_vib(void *arg) {
    (void)arg;
    lichen_haptics_set_enabled(!lichen_haptics_enabled());
    menu_set_on(s_s.vib_row, lichen_haptics_enabled());
    if (lichen_haptics_enabled()) {
        lichen_haptics_play(LICHEN_HAPTIC_TICK); // feel what it was switched to
    }
}

static char const *const VIB_STRENGTH[] = {"Low", "Medium", "High"};

static void cycle_vib_strength(void *arg) {
    (void)arg;
    lichen_haptics_set_strength((lichen_haptic_strength_t)((lichen_haptics_strength() + 1) % 3));
    menu_set_value(s_s.vib_str_row, VIB_STRENGTH[lichen_haptics_strength()]);
    lichen_haptics_play(LICHEN_HAPTIC_TICK); // feel the new strength
}

static void settings_clear_answer(bool yes) {
    if (yes) {
        lichen_notify_clear_all();
        if (s_s.clear_row) {
            menu_set_value(s_s.clear_row, "0");
        }
    }
}

// The row is a DANGER row (menu_set_danger): this runs after the careful question was confirmed
static void settings_clear(void *arg) {
    (void)arg;
    settings_clear_answer(true);
}

static void on_settings_delete(lv_event_t *e) {
    (void)e;
    memset(&s_s, 0, sizeof(s_s));
}

static lv_obj_t *build_settings(void) {
    memset(&s_s, 0, sizeof(s_s));
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Notifications");
    menu_note(menu, "Show notifications from:");
    size_t n = 0;
    for (size_t i = 0; i < lichen_app_count() && n < MAX_SOURCES - 1; i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (!app->id || strcmp(app->id, NOTIFY_APP_ID) == 0) {
            continue;
        }
        s_s.source[n]     = app->id;
        s_s.source_row[n] = menu_add(menu, app->name, "", toggle_source, (void *)(uintptr_t)n);
        menu_set_kind(s_s.source_row[n], MENU_TOGGLE);
        menu_set_on(s_s.source_row[n], lichen_notify_source_enabled(app->id));
        n++;
    }
    s_s.source[n]     = "lichen";
    s_s.source_row[n] = menu_add(menu, "Lichen (system)", "", toggle_source, (void *)(uintptr_t)n);
    menu_set_kind(s_s.source_row[n], MENU_TOGGLE);
    menu_set_on(s_s.source_row[n], lichen_notify_source_enabled("lichen"));
    for (size_t i = 0; i < lichen_app_count(); i++) { // what an app sends, if it can be set (#50)
        lichen_app_t const *app = lichen_app_at(i);
        if (app->notify_settings_open) {
            char label[96];
            snprintf(label, sizeof(label), "%s notifications", app->name);
            menu_set_kind(menu_add(menu, label, "", open_app_notify_settings, (void *)app), MENU_OPEN);
        }
    }
    s_s.dnd_row = menu_add(menu, "Do not disturb", "", toggle_dnd, NULL);
    s_s.led_row = menu_add(menu, "Status LED", "", toggle_led, NULL);
    s_s.vib_row = menu_add(menu, "Vibration", "", toggle_vib, NULL);
    menu_set_kind(s_s.dnd_row, MENU_TOGGLE);
    menu_set_kind(s_s.led_row, MENU_TOGGLE);
    menu_set_kind(s_s.vib_row, MENU_TOGGLE);
    menu_set_on(s_s.dnd_row, lichen_notify_dnd());
    menu_set_on(s_s.led_row, led_enabled());
    menu_set_on(s_s.vib_row, lichen_haptics_enabled());
    s_s.vib_str_row = menu_add(menu, "Vibration strength", VIB_STRENGTH[lichen_haptics_strength()], cycle_vib_strength,
                               NULL);
    menu_set_kind(s_s.vib_str_row, MENU_CYCLE);
    char       count[12];
    ln_stats_t st;
    lichen_notify_stats(&st);
    snprintf(count, sizeof(count), "%u", (unsigned)st.entries);
    s_s.clear_row = menu_add(menu, "Clear all", count, settings_clear, NULL);
    menu_set_danger(s_s.clear_row, "Clear all notifications?", "clear all");
    menu_note(menu, "A source that is off loses its notices: no history, no badge. Do not disturb keeps them but shows "
                    "no pop-ups, except errors (without sound).");
    lv_obj_add_event_cb(screen, on_settings_delete, LV_EVENT_DELETE, NULL);
    return screen;
}

void notify_settings_open(void) {
    shell_open(build_settings, SCREEN_NOTIFY_SETTINGS);
}
