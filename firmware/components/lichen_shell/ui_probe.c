// Bench diagnostics (console only, #67): `fw keys send <seq>` feeds keys to the screen on top and `fw ui` prints what
// is on it, so a screen can be driven and read without anyone at the badge. Tokens: see ui_keys.h.
#include "console_cmd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lichen_sys_ui.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_ui.h"
#include "nav.h"
#include "raw_keys.h"
#include "shell_parts.h"
#include "ui_keys.h"
#include "ui_overflow.h"
#include "ui_remote.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static_assert(LV_KEY_UP == 17 && LV_KEY_DOWN == 18 && LV_KEY_RIGHT == 19 && LV_KEY_LEFT == 20 && LV_KEY_ENTER == 10 &&
                  LV_KEY_ESC == 27 && LV_KEY_NEXT == 9 && LV_KEY_BACKSPACE == 8,
              "ui_keys.c's key codes must match LVGL's");

#define KEY_GAP_MS  250 // lets the shell's async screen changes finish between keys
#define WAIT_MS     1500
#define MAX_KEYS    128
#define MAX_LINES   64
#define LINE_CHARS  96

typedef struct {
    int32_t y, x;
    char    text[LINE_CHARS];
} line_t;

// LVGL context only
static void deliver_key(uint32_t key) {
    if (screen_idle_key()) {
        return; // it woke the dark screen (#43), as a real key would
    }
    if (screen_lock_active()) {
        screen_lock_probe_key(key); // the lock takes every key but feeds the unlock combo (#75, #112)
        return;
    }
    lv_group_t *group = shell_top_group();
    if (UI_KEY_IS_SHAPE(key)) {
        shortcuts_key(UI_KEY_SHAPE_CODE(key), true, raw_keys_test_open()); // as the real key: the OS's, never LVGL's
    } else if (key == LV_KEY_ESC) {
        shell_esc(); // the shell's own Esc handling (it sees the indev's key, which this does not go through)
    } else if (key == LV_KEY_NEXT) {
        if (group) {
            lv_group_focus_next(group);
        }
    } else if (group) {
        lv_group_send_data(group, key);
    }
}

static void describe_focus(lv_obj_t *foc, char *out, size_t cap);

// What "fw ui" reports as the focus; "" for none. LVGL context.
static void current_focus(char *out, size_t cap) {
    out[0]            = 0;
    lv_group_t *group = shell_top_group();
    lv_obj_t   *foc   = group ? lv_group_get_focused(group) : NULL;
    if (foc) {
        describe_focus(foc, out, cap);
    }
}

// The key handlers (and what they build) run on the LVGL task, one key per timer tick: the console task's 4 KB stack
// is too small for them. The console task waits for the end. A failed guard (#190) or the end of the remote key lock
// the sequence started under stops it there: no later key is sent.
typedef struct {
    uint32_t     keys[MAX_KEYS];
    size_t       n, next;
    TaskHandle_t waiter;
    bool         locked;      // the remote key lock was on when the sequence started
    bool         lock_lost;   // ... and ended before it was through, or a needlock found it off
    size_t       failed;      // the guard that failed, 1-based among the guards; 0 = none
    size_t       guards;
    char         focus[LINE_CHARS]; // the focus a failed guard found
    char         text[CONSOLE_WIFI_CMD_MAX]; // the sequence as sent: the guards' texts are read from it
} sequence_t;

static void sequence_finish(lv_timer_t *timer, sequence_t *seq) {
    screen_lock_probe_end();
    xTaskNotifyGive(seq->waiter); // the last gap has passed: the screen has settled
    lv_timer_delete(timer);
}

static void sequence_step(lv_timer_t *timer) {
    sequence_t *seq = lv_timer_get_user_data(timer);
    if (seq->locked && !ui_remote_locked()) {
        seq->lock_lost = true; // the holder took the keys back (or the lock timed out): hands off
        sequence_finish(timer, seq);
        return;
    }
    while (seq->next < seq->n &&
           (UI_KEY_IS_GUARD(seq->keys[seq->next]) || seq->keys[seq->next] == UI_KEY_NEEDLOCK)) {
        uint32_t guard = seq->keys[seq->next++];
        if (guard == UI_KEY_NEEDLOCK) {
            if (!ui_remote_locked()) {
                seq->lock_lost = true;
                sequence_finish(timer, seq);
                return;
            }
            seq->locked = true; // from here on the lock must hold to the end
            continue;
        }
        seq->guards++;
        current_focus(seq->focus, sizeof(seq->focus));
        if (!ui_keys_guard_ok(guard, seq->text, seq->focus)) {
            seq->failed = seq->guards;
            sequence_finish(timer, seq);
            return;
        }
    }
    if (seq->next < seq->n) {
        uint32_t key = seq->keys[seq->next++];
        if (key == UI_KEY_WAIT) {
            lv_timer_set_period(timer, WAIT_MS);
            return;
        }
        deliver_key(key);
        lv_timer_set_period(timer, KEY_GAP_MS);
        return;
    }
    sequence_finish(timer, seq);
}

static void sequence_report(sequence_t const *seq) {
    if (seq->failed) {
        console_reply("keys send: guard %u failed: focus '%s'; %u of %u sent", (unsigned)seq->failed,
                      seq->focus[0] ? seq->focus : "(none)", (unsigned)seq->next, (unsigned)seq->n);
    } else if (seq->lock_lost) {
        console_reply("keys send: stopped, the key lock is off; %u of %u sent", (unsigned)seq->next, (unsigned)seq->n);
    } else {
        console_reply("keys sent: %u", (unsigned)seq->n);
    }
}

// "fw keys send <seq>" (shell.c's keys command hands it over)
void ui_probe_send(char const *seq_text) {
    size_t      pos;
    sequence_t *seq = heap_caps_calloc(1, sizeof(*seq), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!seq) {
        console_reply("keys send: no memory");
        return;
    }
    snprintf(seq->text, sizeof(seq->text), "%s", seq_text);
    seq->n = ui_keys_parse(seq->text, seq->keys, MAX_KEYS, &pos);
    if (seq->n == UI_KEYS_ERR) {
        console_reply("keys send: not understood at offset %u", (unsigned)pos);
        free(seq);
        return;
    }
    seq->locked = ui_remote_locked();
    seq->waiter = xTaskGetCurrentTaskHandle();
    ulTaskNotifyTake(pdTRUE, 0); // a stale notification would end the wait early
    lichen_sys_ui_lock(0);
    lv_timer_t *timer = lv_timer_create(sequence_step, 1, seq);
    lichen_sys_ui_unlock();
    if (!timer) {
        console_reply("keys send: no timer");
        free(seq);
        return;
    }
    uint32_t total_ms = (uint32_t)(seq->n + 2) * (KEY_GAP_MS + WAIT_MS);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(total_ms)) == 0) {
        // Not finished (LVGL stuck?): the timer still owns seq, so leave it be rather than free it under the timer
        console_reply("keys send: timed out");
        return;
    }
    sequence_report(seq);
    free(seq);
}

static bool visible(lv_obj_t *obj) {
    for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_is_hidden(o)) {
            return false;
        }
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a.x2 >= 0 && a.y2 >= 0 && a.x1 < lv_display_get_horizontal_resolution(NULL) &&
           a.y1 < lv_display_get_vertical_resolution(NULL);
}

static void collect(lv_obj_t *obj, line_t *lines, size_t *n) {
    if (*n >= MAX_LINES) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && visible(obj)) {
        char const *text = lv_label_get_text(obj);
        if (text && *text) {
            lv_area_t a;
            lv_obj_get_coords(obj, &a);
            lines[*n].y = a.y1;
            lines[*n].x = a.x1;
            // "* ": a checked button's label (the active tab, a chosen option)
            lv_obj_t *parent  = lv_obj_get_parent(obj);
            bool      checked = parent && lv_obj_has_state(parent, LV_STATE_CHECKED);
            snprintf(lines[*n].text, LINE_CHARS, "%s%s", checked ? "* " : "", text);
            (*n)++;
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        collect(lv_obj_get_child(obj, (int32_t)i), lines, n);
    }
}

static int by_position(void const *a, void const *b) {
    line_t const *l = a, *r = b;
    return l->y != r->y ? (l->y < r->y ? -1 : 1) : (l->x < r->x ? -1 : l->x > r->x);
}

static void first_label(lv_obj_t *obj, char *out, size_t cap) {
    if (out[0]) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        snprintf(out, cap, "%s", lv_label_get_text(obj));
        return;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count && !out[0]; i++) {
        first_label(lv_obj_get_child(obj, (int32_t)i), out, cap);
    }
}

// A list that keeps the group focus on itself marks its highlighted row with LV_STATE_USER_1 (mc_list_style_row, #145):
// the row is what Enter would open, so that is the focus to report
static lv_obj_t *highlighted_child(lv_obj_t *foc) {
    uint32_t count = lv_obj_get_child_count(foc);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(foc, (int32_t)i);
        if (lv_obj_has_state(child, LV_STATE_USER_1)) {
            return child;
        }
    }
    return foc;
}

// What the focus object stands for when its own first label is not it: the home screen holds the focus for all its
// tiles, a tabview for all its tabs (#27)
static void describe_focus(lv_obj_t *foc, char *out, size_t cap) {
    char const *home = (nav_depth() == 1 && foc == lv_screen_active() && shell_home_shown()) ? home_selected_name(foc)
                                                                                             : NULL;
    if (home) {
        snprintf(out, cap, "%s", home);
    } else if (lv_obj_check_type(foc, &lv_tabview_class)) {
        lv_obj_t *button = lv_obj_get_child(lv_tabview_get_tab_bar(foc), (int32_t)lv_tabview_get_tab_active(foc));
        if (button) {
            first_label(button, out, cap);
        }
    } else {
        first_label(highlighted_child(foc), out, cap);
    }
}

static void settle_done(void *arg) {
    xTaskNotifyGive((TaskHandle_t)arg);
}

// Waits until the LVGL task has run what is queued for it (lv_async_call: Enter's screen change, and what that chains
// into), so the report below is of the keys already sent, not one behind
static void settle(void) {
    for (int round = 0; round < 3; round++) {
        ulTaskNotifyTake(pdTRUE, 0);
        lichen_sys_ui_call(settle_done, xTaskGetCurrentTaskHandle(), 0);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300)) == 0) {
            return;
        }
    }
}

// ---- "fw ui overflow" (#133): text that is cut off, runs off the screen or sits under the hint or the status bar ------

#define OVF_TEXT_CHARS 60

// Where the object sits in scrolling content: the page (the ancestor with the most room to scroll) splits the ancestors into those
// that move with the object (inner) and those that stay put (outer); up / down = how far the content can still move
typedef struct {
    ovf_span_t inner, outer;
    int32_t    up, down;
    lv_area_t  scroller; // the page's own box (for "-v"); valid when has_scroller
    bool       has_scroller;
    int        scrollers; // how many ancestors scroll at all
} vgeom_t;

typedef enum { HIT_WIDTH, HIT_HEIGHT, HIT_UNDER, HIT_CLIPPED, HIT_COUNT } hit_kind_t;

static char const *const HIT_NAME[] = {"width", "height", "under-hint", "clipped"};
static_assert(sizeof(HIT_NAME) / sizeof(HIT_NAME[0]) == HIT_COUNT, "HIT_NAME must name every hit kind");

typedef struct {
    hit_kind_t  kind;
    int32_t     a, b, x, y; // width: text px / px available. height: wrapped text / box. clipped: rows lost / height.
                            // under-hint: rows covered / height
    char const *target;     // under-hint: "hint" or "bar"
    vgeom_t     geom;       // "-v": where the object sat in scrolling content (clipped / under-hint only)
    bool        has_geom;
    char        text[OVF_TEXT_CHARS + 4];
} hit_t;

typedef struct {
    hit_t     *hits;
    size_t     n, total; // total counts every hit, hits holds the first MAX_LINES
    bool       in_screen; // walking the active screen: its content must stay off the hint and the bar
    lv_obj_t  *hint_obj;
    bool       has_hint, has_bar;
    ovf_rect_t hint, bar;
    vgeom_t const *geom; // set while the vertical checks of one object run
} ovf_ctx_t;

static ovf_mode_t overflow_mode(lv_label_long_mode_t m) {
    switch (m) {
    case LV_LABEL_LONG_MODE_CLIP:
        return OVF_CLIP;
    case LV_LABEL_LONG_MODE_DOTS:
        return OVF_DOT;
    case LV_LABEL_LONG_MODE_SCROLL:
    case LV_LABEL_LONG_MODE_SCROLL_CIRCULAR:
        return OVF_SCROLL;
    default:
        return OVF_WRAP;
    }
}

// The label's visible box: its coords cut by every ancestor's coords and the screen
static void visible_box(lv_obj_t *obj, lv_area_t *box) {
    lv_obj_get_coords(obj, box);
    for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
        lv_area_t a;
        lv_obj_get_coords(p, &a);
        box->x1 = LV_MAX(box->x1, a.x1);
        box->y1 = LV_MAX(box->y1, a.y1);
        box->x2 = LV_MIN(box->x2, a.x2);
        box->y2 = LV_MIN(box->y2, a.y2);
    }
    box->x1 = LV_MAX(box->x1, 0);
    box->y1 = LV_MAX(box->y1, 0);
    box->x2 = LV_MIN(box->x2, lv_display_get_horizontal_resolution(NULL) - 1);
    box->y2 = LV_MIN(box->y2, lv_display_get_vertical_resolution(NULL) - 1);
}

// Counts the hit; keeps the first MAX_LINES with their text (the label's own, or the first label inside a container)
static void add_hit(ovf_ctx_t *ctx, hit_kind_t kind, lv_obj_t *obj, int32_t a, int32_t b, char const *target) {
    ctx->total++;
    if (ctx->n >= MAX_LINES) {
        return;
    }
    hit_t    *h = &ctx->hits[ctx->n++];
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    h->kind   = kind;
    h->a      = a;
    h->b      = b;
    h->x      = area.x1;
    h->y      = area.y1;
    h->target = target;
    h->has_geom = ctx->geom != NULL;
    if (ctx->geom) {
        h->geom = *ctx->geom;
    }
    h->text[0] = 0;
    first_label(obj, h->text, sizeof(h->text));
    if (!h->text[0]) {
        snprintf(h->text, sizeof(h->text), "(object)");
    }
    h->text[OVF_TEXT_CHARS] = 0;
    for (char *c = h->text; *c; c++) {
        if (*c == '\n' || *c == '\r') {
            *c = ' ';
        }
    }
}

// Width: the text is longer than the room the label has to show it (#133)
static void check_width(ovf_ctx_t *ctx, lv_obj_t *obj, char const *text) {
    lv_area_t a, box;
    lv_obj_get_coords(obj, &a);
    visible_box(obj, &box);
    if (box.x1 > box.x2 || box.y1 > box.y2 || a.y2 < box.y1 || a.y1 > box.y2) {
        return;
    }
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(obj, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(obj, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t have = 0;
    int32_t over = ui_overflow_px(size.x, (ovf_span_t){a.x1, a.x2}, (ovf_span_t){box.x1, box.x2},
                                  overflow_mode(lv_label_get_long_mode(obj)), &have);
    if (over > 0) {
        add_hit(ctx, HIT_WIDTH, obj, size.x, have, NULL);
    }
}

// Height: a wrapping label of fixed height whose wrapped text needs more rows than it has
static void check_wrap_height(ovf_ctx_t *ctx, lv_obj_t *obj, char const *text) {
    int32_t width = lv_obj_get_content_width(obj);
    if (lv_label_get_long_mode(obj) != LV_LABEL_LONG_MODE_WRAP || width <= 0) {
        return;
    }
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(obj, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(obj, LV_PART_MAIN), width, LV_TEXT_FLAG_NONE);
    int32_t box = lv_obj_get_content_height(obj);
    if (ovf_wrap_lost_px(size.y, box) > 0) {
        add_hit(ctx, HIT_HEIGHT, obj, size.y, box, NULL);
    }
}

static bool scrolls(lv_obj_t *o) {
    return lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_dir(o) & LV_DIR_VER) &&
           (lv_obj_get_scroll_bottom(o) > 0 || lv_obj_get_scroll_top(o) > 0);
}

#define MAX_DEPTH 32

// The page is the scrolling ancestor with the most room to scroll, not the nearest: a snug icon box can scroll by a
// few px (its label is taller than it) and would hide that the home screen scrolls the icon into view (#133)
static lv_obj_t *page_scroller(lv_obj_t *obj, int *count) {
    lv_obj_t *cand[MAX_DEPTH];
    int32_t   range[MAX_DEPTH];
    int       n = 0;
    for (lv_obj_t *p = lv_obj_get_parent(obj); p && n < MAX_DEPTH; p = lv_obj_get_parent(p)) {
        if (scrolls(p)) {
            cand[n]    = p;
            range[n++] = LV_MAX(lv_obj_get_scroll_bottom(p), 0) + LV_MAX(lv_obj_get_scroll_top(p), 0);
        }
    }
    *count = n;
    int best = ovf_pick_scroller(range, n);
    return best < 0 ? NULL : cand[best];
}

static void vgeom(lv_obj_t *obj, vgeom_t *g) {
    g->inner        = OVF_NO_LIMIT;
    g->outer        = OVF_NO_LIMIT;
    g->up           = 0;
    g->down         = 0;
    g->has_scroller = false;
    lv_obj_t   *page = page_scroller(obj, &g->scrollers);
    ovf_span_t *clip = &g->inner;
    for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
        lv_area_t a;
        lv_obj_get_coords(p, &a);
        if (p == page) {
            clip            = &g->outer;
            g->up           = LV_MAX(lv_obj_get_scroll_bottom(p), 0);
            g->down         = LV_MAX(lv_obj_get_scroll_top(p), 0);
            g->scroller     = a;
            g->has_scroller = true;
        }
        clip->x1 = LV_MAX(clip->x1, a.y1);
        clip->x2 = LV_MIN(clip->x2, a.y2);
    }
}

// What the user reads or sees: a label with text, or a leaf with a background (bars, swatches)
static bool is_content(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        char const *t = lv_label_get_text(obj);
        return t && *t;
    }
    return lv_obj_get_child_count(obj) == 0 && lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) > LV_OPA_TRANSP;
}

static void check_cover(ovf_ctx_t *ctx, lv_obj_t *obj, lv_area_t const *a, vgeom_t const *g) {
    if (!ctx->in_screen || obj == ctx->hint_obj || !is_content(obj)) {
        return;
    }
    ovf_rect_t r = {a->x1, a->y1, a->x2, a->y2};
    int32_t    h = a->y2 - a->y1 + 1;
    if (ctx->has_hint) {
        int32_t over = ovf_cover_rows(r, g->inner, g->outer, ctx->hint, g->up, g->down);
        if (over > 0) {
            add_hit(ctx, HIT_UNDER, obj, over, h, "hint");
        }
    }
    if (ctx->has_bar) {
        int32_t over = ovf_cover_rows(r, g->inner, g->outer, ctx->bar, g->up, g->down);
        if (over > 0) {
            add_hit(ctx, HIT_UNDER, obj, over, h, "bar");
        }
    }
}

// Clipped: rows no scroll position shows (off the screen, or beyond a parent that does not scroll), then the cover
// check. Returns true for a container that is wholly hidden: reported once, its children are not walked.
static bool check_vertical(ovf_ctx_t *ctx, lv_obj_t *obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t h = a.y2 - a.y1 + 1;
    if (h <= 0 || a.x2 < a.x1) {
        return false;
    }
    vgeom_t g;
    vgeom(obj, &g);
    ctx->geom = &g;
    int32_t hidden    = ovf_hidden_rows((ovf_span_t){a.y1, a.y2}, g.inner, g.outer, g.up, g.down);
    if (lv_obj_check_type(obj, &lv_label_class) &&
        hidden <= ovf_line_slack_px(lv_font_get_line_height(lv_obj_get_style_text_font(obj, LV_PART_MAIN)))) {
        hidden = 0; // the cut rows are the line's leading, the glyphs show whole (an icon letter in a snug box)
    }
    bool    container = lv_obj_get_child_count(obj) > 0 && !lv_obj_check_type(obj, &lv_label_class);
    if (hidden > 0 && (!container || hidden >= h)) {
        add_hit(ctx, HIT_CLIPPED, obj, hidden, h, NULL);
    }
    check_cover(ctx, obj, &a, &g);
    ctx->geom = NULL;
    return container && hidden >= h;
}

static void overflow_walk(ovf_ctx_t *ctx, lv_obj_t *obj, bool root) {
    if (lv_obj_is_hidden(obj)) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        char const *text = lv_label_get_text(obj);
        if (text && *text) {
            check_width(ctx, obj, text);
            check_wrap_height(ctx, obj, text);
        }
    }
    if (!root && check_vertical(ctx, obj)) {
        return;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        overflow_walk(ctx, lv_obj_get_child(obj, (int32_t)i), false);
    }
}

static bool area_rect(lv_obj_t *obj, ovf_rect_t *out) {
    if (!obj || lv_obj_is_hidden(obj)) {
        return false;
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    *out = (ovf_rect_t){a.x1, a.y1, a.x2, a.y2};
    return true;
}

// The active screen (checked against the pinned hint and the status bar), then the top layer (status bar, toast,
// dialogs) and the sys layer. Those draw over the screen on purpose, so they are not checked against the hint.
//   ui overflow: <n>
//     <kind> x,y a/b px ["hint"|"bar" ]"<text>"      kind: width | height | clipped | under-hint (see hit_t)
static void print_geom(vgeom_t const *g) {
    console_reply("    inner %d..%d outer %d..%d up %d down %d scrollers %d", (int)g->inner.x1, (int)g->inner.x2,
                  (int)g->outer.x1, (int)g->outer.x2, (int)g->up, (int)g->down, g->scrollers);
    if (g->has_scroller) {
        console_reply("    page box %d,%d %d,%d", (int)g->scroller.x1, (int)g->scroller.y1, (int)g->scroller.x2,
                      (int)g->scroller.y2);
    } else {
        console_reply("    page none");
    }
}

static void cmd_ui_overflow(bool verbose) {
    settle();
    ovf_ctx_t ctx = {.hits = malloc(sizeof(hit_t) * MAX_LINES)};
    if (!ctx.hits) {
        console_reply("ui overflow: no memory");
        return;
    }
    lichen_sys_ui_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_update_layout(screen); // scroll ranges and sizes are read below
    ctx.hint_obj     = lichen_page_hint_of(screen);
    ctx.has_hint     = area_rect(ctx.hint_obj, &ctx.hint);
    ctx.has_bar      = area_rect(shell_status_bar(), &ctx.bar);
    ctx.in_screen    = true;
    overflow_walk(&ctx, screen, true);
    ctx.in_screen = false;
    overflow_walk(&ctx, lv_layer_top(), true);
    overflow_walk(&ctx, lv_layer_sys(), true);
    lichen_sys_ui_unlock();
    console_reply("ui overflow: %u", (unsigned)ctx.total);
    for (size_t i = 0; i < ctx.n; i++) {
        hit_t const *h = &ctx.hits[i];
        console_reply("  %s %d,%d %d/%d px %s%s\"%s\"", HIT_NAME[h->kind], (int)h->x, (int)h->y, (int)h->a, (int)h->b,
                      h->target ? h->target : "", h->target ? " " : "", h->text);
        if (verbose && h->has_geom) {
            print_geom(&h->geom);
        }
    }
    if (ctx.total > ctx.n) {
        console_reply("  ... %u more not shown", (unsigned)(ctx.total - ctx.n));
    }
    free(ctx.hits);
}

static void cmd_ui(char const *args) {
    if (strncmp(args, "perf", 4) == 0) {
        ui_perf_cmd(args + 4);
        return;
    }
    if (strncmp(args, "overflow", 8) == 0) {
        cmd_ui_overflow(strstr(args + 8, "-v") != NULL);
        return;
    }
    if (strncmp(args, "lockcheck", 9) == 0 && (args[9] == 0 || args[9] == ' ')) {
        lichen_sys_ui_lockcheck_cmd(args + 9); // the debug lock-owner check's hits (#225)
        return;
    }
    if (strncmp(args, "lock", 4) == 0 && (args[4] == 0 || args[4] == ' ')) {
        ui_remote_lock_cmd(args + 4);
        return;
    }
    if (strncmp(args, "open", 4) == 0 && (args[4] == 0 || args[4] == ' ')) {
        ui_remote_open_cmd(args + 4);
        settle();
        return;
    }
    settle();
    line_t *lines = malloc(sizeof(line_t) * MAX_LINES);
    if (!lines) {
        console_reply("ui: no memory");
        return;
    }
    size_t n = 0;
    char   stack[64] = "", focus[LINE_CHARS] = "";
    lichen_sys_ui_lock(0);
    for (int i = 0; i < nav_depth(); i++) {
        size_t used = strlen(stack);
        snprintf(stack + used, sizeof(stack) - used, "%s%d", i ? " > " : "", nav_at(i));
    }
    current_focus(focus, sizeof(focus));
    collect(lv_screen_active(), lines, &n);
    lichen_sys_ui_unlock();
    qsort(lines, n, sizeof(line_t), by_position);
    console_reply("ui stack: %s | focus: %s", stack, focus[0] ? focus : "(none)");
    ui_remote_report();
    for (size_t i = 0; i < n; i++) {
        console_reply("ui   %s", lines[i].text);
    }
    free(lines);
}

void ui_probe_init(void) {
    console_register("ui", "ui [perf [reset | bench [n]] | overflow [-v] | lock [on [s] | off] | open <app> | lockcheck [reset]] (lockcheck: LVGL calls made without the LVGL lock, #225; lock: physical keys off until off, s seconds (default 60, max 600) after the last command, or Esc held 3 s; open: an app as from its home tile, or home; #190. overflow: text cut off, off the screen or under the hint/status bar, #133; -v: the scroll geometry of each hit; screen ids on the stack, the focused row, every visible label; perf: frame timing, #93)", cmd_ui);
}
