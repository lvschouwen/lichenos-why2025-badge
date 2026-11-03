// Calculator (#158): type an expression, see it in decimal, hex and binary; below, a few unit conversions for radio
// work. The arithmetic is calc_eval.c (integers in 64 bits, hex 0x, binary 0b, C operators), the units calc_units.c; this
// file is the screen. What was typed stays until the badge restarts (RAM only, no flash). Only the OS API.
#include "app_icons.h"
#include "calc_eval.h"
#include "calc_units.h"
#include "fonts.h"
#include "lichen_app.h"
#include "menu.h"
#include "shell_parts.h"
#include "text_entry.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define EXPR_MAX 100
#define NUM_MAX  24

// What the user typed, kept across opens
static struct {
    char        expr[EXPR_MAX + 1];
    char        value[NUM_MAX + 1];
    unit_conv_t conv;
} s_c;

// The open screen's widgets; NULL while closed
static struct {
    lv_obj_t *expr_row, *dec, *hex, *bin, *conv_row, *value_row, *unit_result;
} s_w;

static void colour(lv_obj_t *label, uint32_t color) {
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static void show(lv_obj_t *label, char const *kind, char const *text, uint32_t color) {
    char line[160];
    snprintf(line, sizeof(line), "%s%s%s", kind, kind[0] ? "  " : "", text);
    lv_label_set_text(label, line);
    colour(label, color);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
}

static void show_expression(void) {
    int64_t    value;
    size_t     pos;
    calc_err_t err = s_c.expr[0] ? calc_eval(s_c.expr, &value, &pos) : CALC_EMPTY;
    menu_set_value(s_w.expr_row, s_c.expr[0] ? s_c.expr : "none");
    if (err != CALC_OK) {
        char text[120];
        if (err == CALC_EMPTY) {
            snprintf(text, sizeof(text), "%s", calc_err_text(err));
        } else {
            snprintf(text, sizeof(text), "%s (at character %zu)", calc_err_text(err), pos + 1);
        }
        show(s_w.dec, "", text, err == CALC_EMPTY ? THEME_DIM : THEME_RED);
        lv_obj_add_flag(s_w.hex, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_w.bin, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char text[CALC_FMT_MAX];
    calc_fmt_dec(text, sizeof(text), value);
    show(s_w.dec, "Dec", text, THEME_TEXT);
    calc_fmt_hex(text, sizeof(text), value);
    show(s_w.hex, "Hex", text, THEME_TEXT);
    calc_fmt_bin(text, sizeof(text), value);
    show(s_w.bin, "Bin", text, THEME_TEXT);
}

static void show_unit(void) {
    char value[NUM_MAX + 16];
    snprintf(value, sizeof(value), "%s%s%s", s_c.value[0] ? s_c.value : "none", s_c.value[0] ? " " : "",
             s_c.value[0] ? unit_in_name(s_c.conv) : "");
    menu_set_value(s_w.value_row, value);
    menu_set_value(s_w.conv_row, unit_conv_name(s_c.conv));
    double in;
    if (!s_c.value[0]) {
        show(s_w.unit_result, "", "Enter a number to convert.", THEME_DIM);
    } else if (!unit_parse_number(s_c.value, &in)) {
        show(s_w.unit_result, "", "Not a number: type for example 14 or -3.5", THEME_RED);
    } else {
        char       out[48];
        unit_err_t err = unit_convert(s_c.conv, in, out, sizeof(out));
        if (err != UNIT_OK) {
            show(s_w.unit_result, "", unit_err_text(err, s_c.conv), THEME_RED);
        } else {
            char line[96];
            snprintf(line, sizeof(line), "%s %s = %s", s_c.value, unit_in_name(s_c.conv), out);
            show(s_w.unit_result, "", line, THEME_TEXT);
        }
    }
}

static void on_expr_done(char const *text, void *arg) {
    (void)arg;
    snprintf(s_c.expr, sizeof(s_c.expr), "%s", text);
    if (s_w.expr_row) {
        show_expression();
    }
}

static void on_expr(void *arg) {
    (void)arg;
    text_entry_open("Expression", s_c.expr, false, EXPR_MAX, on_expr_done, NULL);
}

static void on_value_done(char const *text, void *arg) {
    (void)arg;
    snprintf(s_c.value, sizeof(s_c.value), "%s", text);
    if (s_w.value_row) {
        show_unit();
    }
}

static void on_value(void *arg) {
    (void)arg;
    char title[40];
    snprintf(title, sizeof(title), "Number in %s", unit_in_name(s_c.conv));
    text_entry_open(title, s_c.value, false, NUM_MAX, on_value_done, NULL);
}

static void on_conv(void *arg) {
    (void)arg;
    s_c.conv = (unit_conv_t)((s_c.conv + 1) % UNIT_COUNT);
    show_unit();
}

static void on_delete(lv_event_t *e) {
    (void)e;
    memset(&s_w, 0, sizeof(s_w));
}

static void open_calc(lv_obj_t *screen) {
    memset(&s_w, 0, sizeof(s_w));
    lv_obj_t *menu = menu_create(screen, "Calculator");
    s_w.expr_row   = menu_add(menu, "Expression", "", on_expr, NULL);
    menu_set_kind(s_w.expr_row, MENU_OPEN);
    s_w.dec = menu_note(menu, "");
    s_w.hex = menu_note(menu, "");
    s_w.bin = menu_note(menu, "");
    menu_note(menu, "Units");
    s_w.conv_row = menu_add(menu, "Convert", "", on_conv, NULL);
    menu_set_kind(s_w.conv_row, MENU_CYCLE);
    s_w.value_row = menu_add(menu, "Number", "", on_value, NULL);
    menu_set_kind(s_w.value_row, MENU_OPEN);
    s_w.unit_result = menu_note(menu, "");
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    show_expression();
    show_unit();
}

static lichen_app_t const calc_app = {
    .name      = "Calculator",
    .id        = "calc",
    .draw_icon = icon_calc,
    .open      = open_calc,
};

void calc_app_register(void) {
    lichen_register_app(&calc_app);
}
