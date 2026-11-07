// A repeater's Status history page (#168): the switch for the scheduled status asks, their interval, when the next one
// goes, and small graphs of the answers kept since start (battery, noise floor, traffic, uptime). The schedule and the
// ring are mesh_rhist_run.c's (mesh_rhist_view: a copy under its mutex, never the mesh lock). Opening the page sends
// nothing; switching on and a shorter interval ask first in the careful dialog (they make the badge transmit on its
// own, or more often: R3).
#include "lichen_mem.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_rhist_run.h"
#include "meshcore_run.h"
#include "meshcore_screens.h"
#include "screen_ids.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define REFRESH_MS 1000
#define CHART_H    110
#define TEXT_MAX   160

typedef struct {
    uint8_t            prefix[RH_PREFIX];
    char               name[MESH_NAME_MAX + 1];
    lv_obj_t          *screen, *toggle, *interval, *next_row, *answers_row, *problem, *msg, *empty;
    lv_obj_t          *note[RH_FIELDS], *chart[RH_FIELDS];
    lv_chart_series_t *ser[RH_FIELDS];
    lv_timer_t        *timer;
    uint16_t           minutes;       // the interval shown (applied when switched on)
    int                shown_on;      // -1: nothing shown yet
    char               shown_interval[16], shown_next[32], shown_answers[64]; // the row values, set only on a change
    uint32_t           drawn_answers; // the graphs show this many answers; UINT32_MAX: nothing drawn yet
    rh_view_t          view;
    rh_sample_t        smp[RH_SAMPLES];
    int32_t            vals[RH_SAMPLES];
} rhv_t;

static char const *const TITLES[RH_FIELDS] = {"Battery", "Noise floor", "Received", "Sent", "Uptime"};

static struct {
    uint8_t prefix[RH_PREFIX];
    char    name[MESH_NAME_MAX + 1];
} want;

static rhv_t *live; // the page that is open (one per repeater; the dialog's answer goes to it if it is still there)
static struct {
    uint8_t  prefix[RH_PREFIX];
    uint16_t minutes;
    bool     was_on; // a shorter interval, not a switch on
} pending;

static void set_text(lv_obj_t *label, char const *text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text); // only on a change: the same text every second would redraw for nothing
    }
}


// The note over a graph: "Battery: 4.10 V now (4.05 V to 4.12 V)"
static void chart_note(rhv_t *v, rh_field_t f, size_t n) {
    int32_t last = RH_NONE, lo = RH_NONE, hi = RH_NONE;
    for (size_t i = 0; i < n; i++) {
        int32_t x = v->vals[i];
        if (x != RH_NONE) {
            last = x;
            lo   = lo == RH_NONE || x < lo ? x : lo;
            hi   = hi == RH_NONE || x > hi ? x : hi;
        }
    }
    char text[TEXT_MAX], a[24], b[24], c[24];
    rh_value_text(f, last, a, sizeof(a));
    rh_value_text(f, lo, b, sizeof(b));
    rh_value_text(f, hi, c, sizeof(c));
    if (last == RH_NONE) {
        snprintf(text, sizeof(text), "%s: %s", TITLES[f],
                 f == RH_BATTERY ? "not reported" : "needs two answers without a restart between them");
    } else if (f == RH_UPTIME) {
        snprintf(text, sizeof(text), "%s: %s now (a drop is a restart)", TITLES[f], a);
    } else {
        snprintf(text, sizeof(text), "%s: %s now (%s to %s)", TITLES[f], a, b, c);
    }
    set_text(v->note[f], text);
}

static void draw_chart(rhv_t *v, rh_field_t f, size_t n) {
    rh_series(v->smp, n, f, v->vals);
    int32_t lo, hi;
    if (!rh_range(v->vals, n, f, &lo, &hi)) {
        lo = 0;
        hi = 10;
    }
    lv_chart_set_axis_range(v->chart[f], LV_CHART_AXIS_PRIMARY_Y, lo, hi);
    lv_chart_set_point_count(v->chart[f], n < 2 ? 2 : (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        lv_chart_set_series_value_by_id(v->chart[f], v->ser[f], (uint32_t)i,
                                        v->vals[i] == RH_NONE ? LV_CHART_POINT_NONE : v->vals[i]);
    }
    if (n < 2) {
        lv_chart_set_series_value_by_id(v->chart[f], v->ser[f], 1, LV_CHART_POINT_NONE);
    }
    chart_note(v, f, n);
}

static void show_graphs(rhv_t *v, rh_slot_t const *s) {
    size_t n = rh_samples(s, v->smp, RH_SAMPLES);
    for (int f = 0; f < RH_FIELDS; f++) {
        lv_obj_set_hidden(v->note[f], n == 0);
        lv_obj_set_hidden(v->chart[f], n == 0);
        if (n) {
            draw_chart(v, (rh_field_t)f, n);
        }
    }
    char text[TEXT_MAX * 2], every[16];
    rh_interval_text(v->minutes, every, sizeof(every));
    if (n) {
        text[0] = 0;
    } else if (s->used && s->on) {
        snprintf(text, sizeof(text), "No answers yet: the graphs fill as the repeater answers, one point every %s.", every);
    } else {
        snprintf(text, sizeof(text),
                 "No answers yet. Switch on Ask for status: the badge asks every %s and draws battery, noise floor, "
                 "traffic and uptime here.",
                 every);
    }
    set_text(v->empty, text);
}

static void set_value(lv_obj_t *row, char *shown, size_t cap, char const *text) {
    if (strcmp(shown, text) != 0) {
        strlcpy(shown, text, cap); // a longer text is shown whole and compared cut: at worst one redraw more
        menu_set_value(row, text);
    }
}

static void show_rows(rhv_t *v) {
    rh_view_t const *w = &v->view;
    rh_slot_t const *s = &w->slot;
    bool             on = w->have && s->on;
    if (w->have) {
        v->minutes = s->interval_min;
    }
    if ((int)on != v->shown_on) {
        menu_set_on(v->toggle, on);
        v->shown_on = on;
    }
    char text[TEXT_MAX], every[16], age[32];
    rh_interval_text(v->minutes, every, sizeof(every));
    set_value(v->interval, v->shown_interval, sizeof(v->shown_interval), every);
    rh_next_text(w->have ? s : NULL, w->asking, w->now_s, text, sizeof(text));
    set_value(v->next_row, v->shown_next, sizeof(v->shown_next), text);
    if (w->have && s->answers) {
        mesh_format_age(w->now_s - s->answer_s, age, sizeof(age));
        snprintf(text, sizeof(text), "%lu of %lu asks, last %s", (unsigned long)s->answers, (unsigned long)s->asks, age);
    } else if (w->have && s->asks) {
        snprintf(text, sizeof(text), "None of %lu asks", (unsigned long)s->asks);
    } else {
        snprintf(text, sizeof(text), "None yet");
    }
    set_value(v->answers_row, v->shown_answers, sizeof(v->shown_answers), text);
    char const *saved = w->unsaved ? "Not saved yet: the badge keeps trying, a restart may undo the last change. " : "";
    if (on && s->why) {
        char wait[16];
        rh_interval_text((uint16_t)(rh_wait_s(s) / 60), wait, sizeof(wait));
        snprintf(text, sizeof(text), "%sLast ask: %s.%s%s%s", saved, s->why,
                 s->misses ? " Asked less often after misses: every " : "", s->misses ? wait : "", s->misses ? " now." : "");
    } else {
        snprintf(text, sizeof(text), "%s", saved);
    }
    set_text(v->problem, text);
}

static void update(rhv_t *v) {
    if (!mesh_rhist_view(v->prefix, &v->view)) {
        set_text(v->empty, "MeshCore is not running.");
        return;
    }
    show_rows(v);
    uint32_t answers = v->view.have ? v->view.slot.answers : 0;
    if (answers != v->drawn_answers || v->view.slot.count == 0) {
        show_graphs(v, &v->view.slot);
        v->drawn_answers = answers;
    }
}

static void refresh(lv_timer_t *timer) {
    rhv_t *v = lv_timer_get_user_data(timer);
    if (lv_screen_active() == v->screen) { // something on top: nothing to redraw for
        update(v);
    }
}

// The outcome of a key on this page; it stays until the next key (the refresh does not touch it)
static void say(rhv_t *v, char const *text) {
    if (v) {
        set_text(v->msg, text);
    }
}

// v: the page to show the outcome on (NULL: none is open any more, the change still holds)
static void apply(rhv_t *v, uint8_t const prefix[RH_PREFIX], bool on, uint16_t minutes) {
    rhs_result_t r = mesh_rhist_set(prefix, on, minutes);
    if (v) {
        update(v);
        say(v, r == RHS_OK ? "" : mesh_rhist_result_text(r));
    }
}

static void on_answer(bool yes) {
    rhv_t *v = live && memcmp(live->prefix, pending.prefix, RH_PREFIX) == 0 ? live : NULL;
    if (yes) {
        apply(v, pending.prefix, true, pending.minutes);
    } else {
        say(v, pending.was_on ? "Interval not changed." : "Not switched on: nothing is sent.");
    }
}

// The careful dialog before a change that makes it transmit on its own or more often (R3)
static void ask(rhv_t *v, uint16_t minutes, bool was_on) {
    live = v; // the page whose row asked: the answer goes there
    memcpy(pending.prefix, v->prefix, RH_PREFIX);
    pending.minutes = minutes;
    pending.was_on  = was_on;
    char t[MESH_NAME_MAX + 80], every[16];
    rh_interval_text(minutes, every, sizeof(every));
    if (was_on) {
        snprintf(t, sizeof(t), "Ask %s for its status more often, every %s?", v->name[0] ? v->name : "(no name)", every);
    } else {
        snprintf(t, sizeof(t), "Ask %s for its status every %s?", v->name[0] ? v->name : "(no name)", every);
    }
    shell_dialog_t const d = {
        .title         = t,
        .count_prefix  = "Each ask is a short request on air, until you switch it off. (", // the sentence before "("
        .seconds       = 30,
        .careful       = true,
        .confirm_label = was_on ? "change" : "switch on",
    };
    if (!shell_dialog_countdown(&d, on_answer)) {
        say(v, mc_busy_note());
    }
}

static void on_toggle(void *arg) {
    rhv_t *v = arg;
    if (v->view.have && v->view.slot.on) {
        apply(v, v->prefix, false, v->minutes); // off at once: it stops transmitting
        return;
    }
    if (!v->view.have && v->view.on_count >= RH_SLOTS) {
        say(v, mesh_rhist_result_text(RHS_FULL));
        return;
    }
    ask(v, v->minutes, false);
}

static void on_interval(void *arg) {
    rhv_t   *v    = arg;
    uint16_t next = rh_interval_next(v->minutes);
    if (v->view.have && v->view.slot.on) {
        if (mesh_rhist_needs_confirm(v->prefix, true, next)) {
            ask(v, next, true); // shorter: more asks on air (R3)
        } else {
            apply(v, v->prefix, true, next); // longer: fewer asks, at once
        }
    } else {
        v->minutes = next;
        char every[16];
        rh_interval_text(v->minutes, every, sizeof(every));
        set_value(v->interval, v->shown_interval, sizeof(v->shown_interval), every); // used when it is switched on
        show_graphs(v, &v->view.slot);
    }
}

static lv_obj_t *add_chart(lv_obj_t *menu, lv_chart_series_t **ser) {
    lv_obj_t *chart = lv_chart_create(menu);
    lv_obj_set_size(chart, LV_PCT(100), CHART_H);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_radius(chart, 8, 0);
    lv_obj_set_style_pad_all(chart, 8, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(THEME_FAINT), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 5, 5, LV_PART_INDICATOR); // a dot per answer: they come many minutes apart
    *ser = lv_chart_add_series(chart, lv_color_hex(THEME_TEAL), LV_CHART_AXIS_PRIMARY_Y);
    return chart;
}

static void on_delete(lv_event_t *e) {
    rhv_t *v = lv_obj_get_user_data(lv_event_get_current_target(e));
    if (!v) {
        return;
    }
    if (live == v) {
        live = NULL;
    }
    if (v->timer) {
        lv_timer_delete(v->timer);
    }
    lichen_free(meshcore_self(), v);
}

static lv_obj_t *add_info(lv_obj_t *menu, char const *label) {
    lv_obj_t *row = menu_add(menu, label, "", NULL, NULL);
    menu_set_kind(row, MENU_INFO);
    return row;
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create_named(screen, "Status history", want.name[0] ? want.name : "(no name)");
    rhv_t    *v      = lichen_alloc(meshcore_self(), sizeof(*v), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!v) {
        menu_note(menu, "Not enough memory to show the history.");
        return screen;
    }
    memcpy(v->prefix, want.prefix, RH_PREFIX);
    snprintf(v->name, sizeof(v->name), "%s", want.name);
    v->screen        = screen;
    v->minutes       = RH_INTERVAL_DEF;
    v->shown_on      = -1;
    v->drawn_answers = UINT32_MAX;
    lv_obj_set_user_data(screen, v);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    v->toggle = menu_add(menu, "Ask for status", "", on_toggle, v);
    menu_set_kind(v->toggle, MENU_TOGGLE);
    v->interval = menu_add(menu, "Interval", "", on_interval, v);
    menu_set_kind(v->interval, MENU_CYCLE);
    v->next_row    = add_info(menu, "Next ask");
    v->answers_row = add_info(menu, "Answers");
    v->problem     = menu_note(menu, "");
    lv_obj_set_style_text_color(v->problem, lv_color_hex(THEME_RED), 0); // with words: "Last ask: no answer..."
    v->msg = menu_note(menu, "");
    lv_obj_set_style_text_color(v->msg, lv_color_hex(THEME_TEXT), 0);
    menu_note(menu, "Asked on air at the interval, at most 3 repeaters at a time. A repeater answers only clients it "
                    "knows: log in to it first. The answers are kept until the badge restarts.");
    v->empty = menu_note(menu, "");
    for (int f = 0; f < RH_FIELDS; f++) {
        v->note[f]  = menu_note(menu, "");
        v->chart[f] = add_chart(menu, &v->ser[f]);
    }
    live     = v;
    v->timer = lv_timer_create(refresh, REFRESH_MS, v);
    update(v); // not the active screen yet, so the timer would skip it
    return screen;
}

void meshcore_rhist_open(uint8_t const prefix[6], char const *name) {
    memcpy(want.prefix, prefix, RH_PREFIX);
    snprintf(want.name, sizeof(want.name), "%s", name ? name : "");
    shell_open_keyed(build, SCREEN_RHIST, shell_key_hash(prefix, RH_PREFIX));
}
