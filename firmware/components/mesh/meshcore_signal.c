// A contact's Signal page (#166): RSSI and SNR of the adverts and direct messages heard from it since start as two small
// graphs, the last hop count, the last advert path and how often it was heard. The samples are the RAM history of
// mesh_task.cpp (mesh_signal_get: a short locked copy, never the mesh lock). Receive only: nothing here transmits.
#include "lichen_mem.h"
#include "meshcore_run.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "meshcore_screens.h"
#include "screen_ids.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define REFRESH_MS 1000
#define CHART_H    150
#define TEXT_MAX   128

enum { ROW_LAST, ROW_SIGNAL, ROW_HOPS, ROW_HEARD, ROWS };

static char const *const LABELS[ROWS] = {"Last heard", "Last signal", "Last hops", "Heard"};

typedef struct {
    uint8_t            prefix[SIG_PREFIX];
    lv_obj_t          *screen, *row[ROWS], *path, *empty, *rssi_note, *snr_note, *rssi_chart, *snr_chart;
    lv_chart_series_t *rssi_ser, *snr_ser;
    lv_timer_t        *timer;
    uint32_t           drawn_heard; // the graph shows this many packets; UINT32_MAX: nothing drawn yet
    char               shown[ROWS][TEXT_MAX], shown_path[TEXT_MAX];
} signal_t;

// What meshcore_signal_open wants built: read once by build, which runs under the LVGL lock and takes no arguments
static struct {
    uint8_t prefix[SIG_PREFIX];
    char    name[MESH_NAME_MAX + 1];
} want;

static void show_row(signal_t *s, int i, char const *text) {
    if (strcmp(s->shown[i], text) != 0) {
        strlcpy(s->shown[i], text, TEXT_MAX);
        menu_set_value(s->row[i], text);
    }
}

static void draw_chart(lv_obj_t *chart, lv_chart_series_t *ser, lv_obj_t *note, char const *what, sig_sample_t const *smp,
                       size_t n, sig_field_t field, char const *unit) {
    int lo, hi;
    sig_range(smp, n, field, &lo, &hi);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
    lv_chart_set_point_count(chart, n < 2 ? 2 : (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        lv_chart_set_series_value_by_id(chart, ser, (uint32_t)i, sig_value(&smp[i], field));
    }
    if (n < 2) {
        lv_chart_set_series_value_by_id(chart, ser, 1, sig_value(&smp[0], field));
    }
    char text[TEXT_MAX];
    snprintf(text, sizeof(text), "%s, last %u heard, %d to %d %s", what, (unsigned)n, lo, hi, unit);
    lichen_label_set(note, text);
}

static void show_empty(signal_t *s, bool empty) {
    lichen_label_set(s->empty, empty ? (mesh_running() ? "Nothing heard from this contact since the badge started. This page "
                                                 "fills as its adverts and messages arrive."
                                               : "MeshCore is not running.")
                             : "");
    for (lv_obj_t **o = (lv_obj_t *[]){s->rssi_chart, s->snr_chart, s->rssi_note, s->snr_note, s->path, NULL}; *o; o++) {
        if (empty) {
            lv_obj_set_hidden(*o, true);
        } else {
            lv_obj_set_hidden(*o, false);
        }
    }
}

static void show_rows(signal_t *s, sig_contact_t const *c, sig_sample_t const *last) {
    char text[TEXT_MAX], age[24];
    uint32_t now = mesh_uptime_s();
    mesh_format_age(now > last->at_s ? now - last->at_s : 0, age, sizeof(age));
    snprintf(text, sizeof(text), "%s, %s", age, sig_kind_name(last->kind));
    show_row(s, ROW_LAST, text);
    char rssi[24], snr[24];
    snprintf(rssi, sizeof(rssi), "%d dBm", sig_value(last, SIG_RSSI));
    snprintf(snr, sizeof(snr), "%.1f dB SNR", last->snr_x10 / 10.0);
    snprintf(text, sizeof(text), "%s, %s", rssi, snr);
    show_row(s, ROW_SIGNAL, text);
    sig_hops_text(last->hops, text, sizeof(text));
    show_row(s, ROW_HOPS, text);
    sig_heard_text(c->heard, text, sizeof(text));
    show_row(s, ROW_HEARD, text + strlen("Heard ")); // the row is already called Heard
}

static void update(signal_t *s) {
    sig_contact_t c;
    if (!mesh_signal_get(s->prefix, &c) || c.count == 0) {
        if (s->drawn_heard != 0) {
            show_empty(s, true);
            for (int i = 0; i < ROWS; i++) {
                show_row(s, i, "-");
            }
            s->drawn_heard = 0;
        }
        return;
    }
    sig_sample_t smp[SIG_SAMPLES];
    size_t       n = sig_samples(&c, smp, SIG_SAMPLES);
    show_rows(s, &c, &smp[n - 1]); // the age moves every second
    if (c.heard == s->drawn_heard) {
        return;
    }
    show_empty(s, false);
    draw_chart(s->rssi_chart, s->rssi_ser, s->rssi_note, "Signal strength", smp, n, SIG_RSSI, "dBm");
    draw_chart(s->snr_chart, s->snr_ser, s->snr_note, "Signal to noise", smp, n, SIG_SNR, "dB");
    char path[TEXT_MAX], text[TEXT_MAX + 24];
    sig_path_text(&c, path, sizeof(path));
    snprintf(text, sizeof(text), "Last advert path: %s", path);
    lichen_label_set(s->path, text);
    s->drawn_heard = c.heard;
}

static void refresh(lv_timer_t *timer) {
    signal_t *s = lv_timer_get_user_data(timer);
    if (lv_screen_active() == s->screen) { // something on top: nothing to redraw for
        update(s);
    }
}

static lv_obj_t *add_chart(lv_obj_t *menu, lv_chart_series_t **ser, uint32_t color) {
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
    lv_obj_set_style_size(chart, 5, 5, LV_PART_INDICATOR); // a dot per packet: they come minutes apart
    *ser = lv_chart_add_series(chart, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y);
    return chart;
}

static void on_delete(lv_event_t *e) {
    signal_t *s = lv_obj_get_user_data(lv_event_get_current_target(e));
    if (!s) {
        return;
    }
    if (s->timer) {
        lv_timer_delete(s->timer);
    }
    lichen_free(meshcore_ctx(), s);
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create_named(screen, "Signal", want.name[0] ? want.name : "(no name)");
    signal_t *s      = lichen_alloc(meshcore_ctx(), sizeof(*s), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!s) {
        menu_note(menu, "Not enough memory to show the signal.");
        return screen;
    }
    memcpy(s->prefix, want.prefix, SIG_PREFIX);
    s->screen      = screen;
    s->drawn_heard = UINT32_MAX;
    lv_obj_set_user_data(screen, s);
    lichen_ui_event_add(screen, on_delete, LV_EVENT_DELETE, NULL);
    // Read-only rows: with no selectable row the menu takes the focus, opens at the top and Up/Down scroll it (#174)
    for (int i = 0; i < ROWS; i++) {
        s->row[i] = menu_add(menu, LABELS[i], "", NULL, NULL);
        menu_set_kind(s->row[i], MENU_INFO);
    }
    s->empty = menu_note(menu, "");
    s->path  = menu_note(menu, "");
    s->rssi_note  = menu_note(menu, "");
    s->rssi_chart = add_chart(menu, &s->rssi_ser, THEME_TEAL);
    s->snr_note   = menu_note(menu, "");
    s->snr_chart  = add_chart(menu, &s->snr_ser, THEME_AMBER);
    s->timer      = lichen_ui_timer_create(refresh, REFRESH_MS, s);
    update(s); // not the active screen yet, so the timer would skip it
    return screen;
}

void meshcore_signal_open(uint8_t const prefix[6], char const *name) {
    memcpy(want.prefix, prefix, SIG_PREFIX);
    snprintf(want.name, sizeof(want.name), "%s", name ? name : "");
    shell_open_keyed(build, SCREEN_MC_SIGNAL, shell_key_hash(prefix, SIG_PREFIX));
}
