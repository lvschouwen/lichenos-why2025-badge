// Host test: the gate forwards lichen_ui.h / lichen_keys.h to the shell's table, with a defined answer before (#221)
#include "lichen_ui_ops.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// What the fake shell saw: the last call's name and arguments
static struct {
    char const *call;
    int         n;
    lv_obj_t   *obj;
    int         id;
    uint32_t    key;
    int32_t     dy;
    void const *fn;
    void       *arg;
} seen;

static lv_obj_t *const OBJ    = (lv_obj_t *)0x1000;
static lv_obj_t *const SCREEN = (lv_obj_t *)0x2000;
static int             token;

static void note(char const *call) {
    seen.call = call;
    seen.n++;
}

static lv_obj_t *build(void) {
    return OBJ;
}
static bool esc_fn(void *arg) {
    (void)arg;
    return true;
}
static void void_fn(void *arg) {
    (void)arg;
}
static void line2_fn(lv_obj_t *line2) {
    (void)line2;
}
static void raw_fn(lichen_raw_key_t const *key, void *arg) {
    (void)key;
    (void)arg;
}
static void done_fn(bool confirmed) {
    (void)confirmed;
}

static void f_home(void) { note("home"); }
static void f_back(void) { note("back"); }
static void f_open_keyed(lv_obj_t *(*b)(void), int id, uint32_t key) {
    note("open_keyed");
    seen.fn  = (void const *)b;
    seen.id  = id;
    seen.key = key;
}
static unsigned f_reuse_count(void) { note("reuse_count"); return 7; }
static void     f_count_reuse(void) { note("count_reuse"); }
static bool     f_screen_open(int id, uint32_t key) {
    note("screen_open");
    seen.id  = id;
    seen.key = key;
    return true;
}
static void f_on_esc(bool (*fn)(void *), void *arg) {
    note("on_esc");
    seen.fn  = (void const *)fn;
    seen.arg = arg;
}
static void f_on_search(void (*fn)(void *), void *arg) {
    note("on_search");
    seen.fn  = (void const *)fn;
    seen.arg = arg;
}
static void f_scroll_by(lv_obj_t *obj, int32_t dy) {
    note("scroll_by");
    seen.obj = obj;
    seen.dy  = dy;
}
static lv_obj_t *f_page_create(void) { note("page_create"); return SCREEN; }
static void      f_scroll_tame(lv_obj_t *obj) { note("scroll_tame"); seen.obj = obj; }
static void      f_no_arrow(lv_obj_t *obj) { note("no_arrow"); seen.obj = obj; }
static bool      f_back_then(void (*then)(void *), void *arg) {
    note("back_then");
    seen.fn  = (void const *)then;
    seen.arg = arg;
    return true;
}
static bool f_back_to(lv_obj_t *screen, void (*then)(void *), void *arg) {
    note("back_to");
    seen.obj = screen;
    seen.fn  = (void const *)then;
    seen.arg = arg;
    return true;
}
static lv_obj_t *f_stack_screen(int from_top) { note("stack_screen"); seen.id = from_top; return SCREEN; }
static void      f_style_screen(lv_obj_t *screen) { note("style_screen"); seen.obj = screen; }
static bool      f_dialog(shell_dialog_t const *d, shell_dialog_fn done) {
    note("dialog");
    seen.arg = (void *)d;
    seen.fn  = (void const *)done;
    return true;
}
static void f_dialog_close(void) { note("dialog_close"); }
static bool f_locked(void) { note("locked"); return true; }
static bool f_station(lv_obj_t *screen, void (*update)(lv_obj_t *)) {
    note("station");
    seen.obj = screen;
    seen.fn  = (void const *)update;
    return true;
}
static bool f_date(char *out, size_t cap) {
    note("date");
    snprintf(out, cap, "Fri 7 Nov");
    return true;
}
static bool f_take(lichen_raw_key_fn fn, void *arg) {
    note("take");
    seen.fn  = (void const *)fn;
    seen.arg = arg;
    return true;
}
static void f_release(lichen_raw_key_fn fn, void *arg) {
    note("release");
    seen.fn  = (void const *)fn;
    seen.arg = arg;
}

static lichen_ui_ops_t const full = {
    .home = f_home, .back = f_back, .open_keyed = f_open_keyed, .open_reuse_count = f_reuse_count,
    .open_count_reuse = f_count_reuse, .screen_open = f_screen_open, .on_esc = f_on_esc, .on_search = f_on_search,
    .scroll_by = f_scroll_by, .page_create = f_page_create, .scroll_tame = f_scroll_tame,
    .no_arrow_scroll = f_no_arrow, .back_then = f_back_then, .back_to = f_back_to, .stack_screen = f_stack_screen,
    .style_screen = f_style_screen, .dialog_countdown = f_dialog, .dialog_close = f_dialog_close,
    .screen_locked = f_locked, .station_clock = f_station, .date_text = f_date, .keys_raw_take = f_take,
    .keys_raw_release = f_release,
};

// Every call with no shell behind it: its "not up" answer, and no call reaches anything
static void not_up(void) {
    int before = seen.n;
    shell_home();
    shell_back();
    shell_open(build, 3);
    shell_open_keyed(build, 3, 9);
    CHECK(shell_open_reuse_count() == 0);
    shell_open_count_reuse();
    CHECK(!shell_screen_open(3, 0));
    shell_on_esc(esc_fn, &token);
    shell_on_search(void_fn, &token);
    shell_scroll_by(OBJ, 10);
    CHECK(shell_page_create() == NULL);
    shell_scroll_tame(OBJ);
    shell_no_arrow_scroll(OBJ);
    CHECK(!shell_back_then(void_fn, &token));
    CHECK(!shell_back_to(SCREEN, void_fn, &token));
    CHECK(shell_stack_screen(0) == NULL);
    shell_style_screen(SCREEN);
    shell_dialog_t const d = {.title = "t", .seconds = 5};
    CHECK(!shell_dialog_countdown(&d, done_fn));
    shell_dialog_close();
    CHECK(!lichen_screen_locked());
    CHECK(!lichen_station_clock(SCREEN, line2_fn));
    char date[16] = "stale";
    CHECK(!lichen_date_text(date, sizeof(date)) && date[0] == '\0');
    CHECK(!lichen_date_text(NULL, 0)); // nothing to write into: still no crash
    CHECK(!lichen_keys_raw_take(raw_fn, &token));
    lichen_keys_raw_release(raw_fn, &token);
    CHECK(seen.n == before);
}

static void forwards_nav(void) {
    shell_home();
    CHECK(strcmp(seen.call, "home") == 0);
    shell_back();
    CHECK(strcmp(seen.call, "back") == 0);
    shell_open(build, 4); // shell_open is open_keyed with key 0
    CHECK(strcmp(seen.call, "open_keyed") == 0 && seen.fn == (void const *)build && seen.id == 4 && seen.key == 0);
    shell_open_keyed(build, 5, 77);
    CHECK(seen.id == 5 && seen.key == 77);
    CHECK(shell_open_reuse_count() == 7);
    shell_open_count_reuse();
    CHECK(strcmp(seen.call, "count_reuse") == 0);
    CHECK(shell_screen_open(6, 8) && seen.id == 6 && seen.key == 8);
    shell_on_esc(esc_fn, &token);
    CHECK(strcmp(seen.call, "on_esc") == 0 && seen.fn == (void const *)esc_fn && seen.arg == &token);
    shell_on_search(void_fn, &token);
    CHECK(strcmp(seen.call, "on_search") == 0 && seen.fn == (void const *)void_fn);
    shell_scroll_by(OBJ, -24);
    CHECK(strcmp(seen.call, "scroll_by") == 0 && seen.obj == OBJ && seen.dy == -24);
    CHECK(shell_page_create() == SCREEN);
    shell_scroll_tame(SCREEN);
    CHECK(strcmp(seen.call, "scroll_tame") == 0 && seen.obj == SCREEN);
    shell_no_arrow_scroll(OBJ);
    CHECK(strcmp(seen.call, "no_arrow") == 0 && seen.obj == OBJ);
    CHECK(shell_back_then(void_fn, &token) && strcmp(seen.call, "back_then") == 0 && seen.arg == &token);
    CHECK(shell_back_to(SCREEN, void_fn, &token) && seen.obj == SCREEN && seen.fn == (void const *)void_fn);
    CHECK(shell_stack_screen(2) == SCREEN && seen.id == 2);
    shell_style_screen(OBJ);
    CHECK(strcmp(seen.call, "style_screen") == 0 && seen.obj == OBJ);
}

static void forwards_rest(void) {
    shell_dialog_t const d = {.title = "t", .seconds = 5};
    CHECK(shell_dialog_countdown(&d, done_fn) && seen.arg == (void *)&d && seen.fn == (void const *)done_fn);
    shell_dialog_close();
    CHECK(strcmp(seen.call, "dialog_close") == 0);
    CHECK(lichen_screen_locked());
    CHECK(lichen_station_clock(SCREEN, line2_fn) && seen.obj == SCREEN && seen.fn == (void const *)line2_fn);
    char date[16];
    CHECK(lichen_date_text(date, sizeof(date)) && strcmp(date, "Fri 7 Nov") == 0);
    CHECK(lichen_keys_raw_take(raw_fn, &token) && strcmp(seen.call, "take") == 0 && seen.arg == &token);
    lichen_keys_raw_release(raw_fn, &token);
    CHECK(strcmp(seen.call, "release") == 0 && seen.fn == (void const *)raw_fn);
}

// The key hash is the gate's own (pure): the same before the shell is up as after, never SHELL_KEY_UNIQUE
static void key_hash(void) {
    CHECK(shell_key_hash("ab", 2) != shell_key_hash("ba", 2));
    CHECK(shell_key_hash("ab", 2) == shell_key_hash("ab", 2));
    CHECK(shell_key_hash("", 0) == 2166136261u); // FNV-1a's offset basis
    // 0xf0 0x9a 0xee 0x00 0xcc hashes to exactly SHELL_KEY_UNIQUE under FNV-1a: it is remapped, never handed out
    CHECK(shell_key_hash("\xf0\x9a\xee\x00\xcc", 5) == 0);
}

int main(void) {
    key_hash();
    not_up(); // before the shell sets its table (the steps before START_SHELL's boot screen)
    lichen_ui_ops_t const empty = {0};
    lichen_ui_ops_set(&empty); // a table with members left out answers as no table
    not_up();
    lichen_ui_ops_set(&full);
    forwards_nav();
    forwards_rest();
    lichen_ui_ops_set(&full); // the shell sets it again in shell_start: nothing changes
    CHECK(shell_open_reuse_count() == 7);
    lichen_ui_ops_set(NULL);
    not_up();
    key_hash();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ui_ops_test ok\n");
    return 0;
}
