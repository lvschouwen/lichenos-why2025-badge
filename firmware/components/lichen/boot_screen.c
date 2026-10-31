// The boot screen (#66). Calm by default: logo, name, a thin progress bar, one sentence, the Lichen version. Any key
// shows the details instead (boot-screen.html option C "hybrid": a segmented bar with one labelled segment per
// start-up step, the current step large, the last log lines, uptime) and a failed step switches to them by itself.
// The keypad is attached after the C6 comes up (main.c), so the key works from then on, not during the first steps.
// Shows nothing app-specific (no MeshCore version or node id): the OS speaks only for itself.
// Reads boot_progress_view() only. Moves on to home by itself 2 s after the last step ends (5 s after a failure).
#include "boot_progress.h"
#include "boot_text.h"
#include "crash.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "log_tap.h"
#include "logo.h"
#include "shell_parts.h"
#include "fonts.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define TICK_MS        100
#define HOME_AFTER_MS  2000 // after the last step ends
#define HOME_AFTER_FAIL_MS 5000 // after a failure, so the reason can be read
#define PULSE_MS       1200 // one breath of the current segment
#define SHOW_LOG_LINES 5

// The mockup's short names, so eight fit under the bar at a readable size
static char const *const short_labels[BOOT_STEP_COUNT] = {"stor", "disp", "psram", "c6", "wifi", "clock", "lora", "apps"};
static char const *const doing[BOOT_STEP_COUNT]        = {
    "mounting storage", "starting the display", "checking PSRAM", "checking the C6",
    "joining Wi-Fi",    "setting the clock",    "starting LoRa",  "starting apps",
};

static lv_obj_t  *segments[BOOT_STEP_COUNT];
static lv_obj_t  *seg_labels[BOOT_STEP_COUNT];
static lv_obj_t  *step_text;
static lv_obj_t  *step_pct;
static lv_obj_t  *failures;
static lv_obj_t  *log_labels[SHOW_LOG_LINES];
static lv_obj_t  *uptime_label;
static lv_obj_t  *calm;       // the default view
static lv_obj_t  *details;    // the technical view
static lv_obj_t  *calm_bar;
static lv_obj_t  *calm_text;
static lv_obj_t  *version_label;
static lv_group_t *group;     // so the keypad reaches this screen
static bool       show_details;
static bool       failure_shown; // the automatic switch to details happens once, the key can still switch back
static lv_timer_t *timer;
static uint32_t   shown_generation;
static bool       generation_shown;
static uint32_t   apps_ended_at; // lv_tick of the first refresh that saw the apps step over, 0 before
static bool       leaving;
static bool       failed;
static boot_view_t view; // the LVGL task's copy

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color, lv_font_t const *font) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *add_box(lv_obj_t *parent, lv_flex_flow_t flow) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    return box;
}

static void set_text(lv_obj_t *label, char const *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static bool ended(step_state_t state) {
    return state == STEP_DONE || state == STEP_FAILED || state == STEP_SKIPPED;
}

// The step to name in large text: the one begun last while it runs, else any other still running
static int running_step(void) {
    if (view.state[view.current] == STEP_ACTIVE) {
        return view.current;
    }
    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        if (view.state[i] == STEP_ACTIVE) {
            return i;
        }
    }
    return -1;
}

static void show_steps(void) {
    static uint32_t const seg_color[]   = {THEME_TRACK, THEME_AMBER, THEME_AMBER, THEME_RED, THEME_FAINT};
    static uint32_t const label_color[] = {THEME_DIM, THEME_AMBER, THEME_DIM, THEME_RED, THEME_DIM};
    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        lv_obj_set_style_bg_color(segments[i], lv_color_hex(seg_color[view.state[i]]), 0);
        lv_obj_set_style_bg_opa(segments[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(seg_labels[i], lv_color_hex(label_color[view.state[i]]), 0);
    }

    int running = running_step();
    if (running >= 0) {
        set_text(step_text, running == BOOT_C6 && view.pct >= 0 ? "flashing the C6" : doing[running]);
        lv_obj_set_style_text_color(step_text, lv_color_hex(THEME_TEXT), 0);
        char pct[16] = "";
        if (running == (int)view.current && view.pct >= 0) {
            snprintf(pct, sizeof(pct), "%d%%", view.pct); // the C6 counts 0-100 once per image (3 images)
        }
        set_text(step_pct, pct);
    } else if (ended(view.state[BOOT_APPS])) {
        set_text(step_text, "ready");
        lv_obj_set_style_text_color(step_text, lv_color_hex(THEME_TEAL), 0);
        set_text(step_pct, "");
    }

    // The calm view: the bar and one sentence
    lv_bar_set_value(calm_bar, boot_overall_pct(&view), LV_ANIM_OFF);
    if (running >= 0) {
        char buf[48];
        set_text(calm_text, boot_step_sentence((boot_step_t)running, running == (int)view.current ? view.pct : -1, buf, sizeof(buf)));
    } else if (ended(view.state[BOOT_APPS])) {
        set_text(calm_text, "Ready");
    }

    // A failed step keeps its reason on screen (boot carries on where it can)
    char text[BOOT_STEP_COUNT * (BOOT_REASON_LEN + 16)] = "";
    size_t used = 0;
    for (int i = 0; i < BOOT_STEP_COUNT && used < sizeof(text); i++) {
        if (view.state[i] == STEP_FAILED) {
            used += snprintf(text + used, sizeof(text) - used, "%s%s failed: %s", used ? "\n" : "",
                             boot_progress_label((boot_step_t)i), view.reason[i][0] ? view.reason[i] : "?");
        }
    }
    set_text(failures, text);
    lv_obj_set_hidden(failures, used == 0);
}

static void apply_view(void) {
    lv_obj_set_hidden(calm, show_details);
    lv_obj_set_hidden(version_label, show_details);
    lv_obj_set_hidden(details, !show_details);
    lv_obj_set_hidden(uptime_label, !show_details);
}

static void on_key(lv_event_t *e) {
    (void)e;
    show_details = !show_details;
    apply_view();
}

static void show_log(void) {
    static char lines[SHOW_LOG_LINES][LOG_TAP_LINE_LEN]; // LVGL task only
    size_t      n = log_tap_last(lines, SHOW_LOG_LINES);
    for (size_t i = 0; i < SHOW_LOG_LINES; i++) {
        set_text(log_labels[i], i < n ? lines[i] : "");
    }
}

static void tick(lv_timer_t *t) {
    (void)t;
    if (leaving) {
        return; // home is fading in, this screen goes when it is done
    }
    boot_progress_view(&view);
    if (!generation_shown || view.generation != shown_generation) {
        show_steps();
        shown_generation = view.generation;
        generation_shown = true;
    }

    // The running segment breathes
    uint32_t phase = lv_tick_get() % PULSE_MS;
    uint32_t level = phase < PULSE_MS / 2 ? phase : PULSE_MS - phase; // 0 .. PULSE_MS/2
    lv_opa_t opa   = (lv_opa_t)(LV_OPA_40 + (LV_OPA_COVER - LV_OPA_40) * level / (PULSE_MS / 2));
    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        if (view.state[i] == STEP_ACTIVE) {
            lv_obj_set_style_bg_opa(segments[i], opa, 0);
        }
    }

    show_log();

    failed = false;
    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        failed = failed || view.state[i] == STEP_FAILED;
    }
    if (failed && !failure_shown) {
        failure_shown = true;
        show_details  = true;
        apply_view();
    }

    char     text[32];
    uint64_t ms = (uint64_t)esp_timer_get_time() / 1000;
    snprintf(text, sizeof(text), "uptime %lu.%lu s", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100));
    set_text(uptime_label, text);

    if (ended(view.state[BOOT_APPS])) {
        uint32_t now = lv_tick_get();
        if (!apps_ended_at) {
            apps_ended_at = now ? now : 1;
        } else if (lv_tick_diff(now, apps_ended_at) >= (failed ? HOME_AFTER_FAIL_MS : HOME_AFTER_MS)) {
            leaving = true;
            shell_home();
        }
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    if (group) {
        lv_group_delete(group); // LVGL detaches it from the keypad
        group = NULL;
    }
}

lv_group_t *boot_screen_group(void) {
    return group;
}

lv_obj_t *boot_screen_create(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    shell_style_screen(screen);
    lv_obj_set_style_pad_all(screen, THEME_PAD, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(screen, false);

    // The calm view: logo, name, a thin bar, one sentence
    calm = lv_obj_create(screen);
    lv_obj_remove_style_all(calm);
    lv_obj_set_size(calm, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(calm, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(calm, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(calm, 20, 0);
    lv_obj_set_scrollable(calm, false);
    lv_obj_set_clickable(calm, false);
    logo_create(calm, 100);
    lv_obj_t *calm_name = add_label(calm, "lichen", THEME_TEXT, lichen_font(LICHEN_FONT_LARGE));
    lv_obj_set_style_text_letter_space(calm_name, 6, 0);
    calm_bar = lv_bar_create(calm);
    lv_obj_set_size(calm_bar, LV_PCT(50), 8);
    lv_obj_set_style_margin_top(calm_bar, 16, 0);
    lv_obj_set_style_bg_color(calm_bar, lv_color_hex(THEME_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(calm_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(calm_bar, lv_color_hex(THEME_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(calm_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(calm_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(calm_bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(calm_bar, 0, 100);
    calm_text = add_label(calm, "Starting...", THEME_TEXT, NULL);
    if (crash_recovered()) {
        add_label(calm, "Recovered from a crash", THEME_AMBER, NULL);
    }
    version_label = add_label(screen, esp_app_get_description()->version, THEME_DIM, NULL);
    lv_obj_set_ignore_layout(version_label, true);
    lv_obj_align(version_label, LV_ALIGN_BOTTOM_MID, 0, 0);

    // The details view: today's technical screen
    details = add_box(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *header = add_box(details, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 24, 0);
    logo_create(header, 100);
    lv_obj_t *names = lv_obj_create(header);
    lv_obj_remove_style_all(names);
    lv_obj_set_size(names, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(names, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(names, 4, 0);
    lv_obj_t *name = add_label(names, "lichen", THEME_TEXT, lichen_font(LICHEN_FONT_LARGE));
    lv_obj_set_style_text_letter_space(name, 6, 0);
    add_label(names, esp_app_get_description()->version, THEME_DIM, NULL);

    // The segmented bar and its labels
    lv_obj_t *bar = add_box(details, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_margin_top(bar, 44, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_t *names_row = add_box(details, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_margin_top(names_row, 8, 0);
    lv_obj_set_style_pad_column(names_row, 6, 0);
    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        segments[i] = lv_obj_create(bar);
        lv_obj_remove_style_all(segments[i]);
        lv_obj_set_height(segments[i], 20);
        lv_obj_set_flex_grow(segments[i], 1);
        lv_obj_set_style_radius(segments[i], 3, 0);
        lv_obj_set_style_bg_color(segments[i], lv_color_hex(THEME_TRACK), 0);
        lv_obj_set_style_bg_opa(segments[i], LV_OPA_COVER, 0);

        seg_labels[i] = add_label(names_row, short_labels[i], THEME_DIM, NULL);
        lv_obj_set_flex_grow(seg_labels[i], 1);
        lv_obj_set_width(seg_labels[i], 1); // flex-grow shares the row out evenly from here
        lv_label_set_long_mode(seg_labels[i], LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(seg_labels[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    // The current step, large
    lv_obj_t *step = add_box(details, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_margin_top(step, 40, 0);
    lv_obj_set_style_pad_column(step, 16, 0);
    lv_obj_set_flex_align(step, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_font(step, lichen_font(LICHEN_FONT_LARGE), 0);
    add_label(step, LV_SYMBOL_RIGHT, THEME_AMBER, NULL);
    step_text  = add_label(step, "starting", THEME_TEXT, NULL);
    step_pct   = add_label(step, "", THEME_AMBER, NULL);

    failures = add_label(details, "", THEME_RED, NULL);
    lv_obj_set_width(failures, LV_PCT(100));
    lv_label_set_long_mode(failures, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_margin_top(failures, 16, 0);
    lv_obj_set_hidden(failures, true);

    if (crash_recovered()) {
        lv_obj_t *crash = add_label(details, "Recovered from a crash - see fw crash", THEME_AMBER, NULL);
        lv_obj_set_style_margin_top(crash, 16, 0);
    }

    // The last log lines, dimmed, one row each
    lv_obj_t *log = add_box(details, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_margin_top(log, 28, 0);
    lv_obj_set_style_pad_row(log, 2, 0);
    for (int i = 0; i < SHOW_LOG_LINES; i++) {
        log_labels[i] = add_label(log, "", THEME_DIM, NULL);
        lv_obj_set_size(log_labels[i], LV_PCT(100), lv_font_get_line_height(lichen_font(LICHEN_FONT_SMALL)));
        lv_label_set_long_mode(log_labels[i], LV_LABEL_LONG_MODE_DOTS);
    }

    // Uptime in the bottom corner of the details
    uptime_label = add_label(screen, "", THEME_DIM, NULL);
    lv_obj_set_ignore_layout(uptime_label, true);
    lv_obj_align(uptime_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    group = lv_group_create();
    lv_group_add_obj(group, screen);
    lv_obj_add_event_cb(screen, on_key, LV_EVENT_KEY, NULL);
    show_details  = false;
    failure_shown = false;
    failed        = false;
    apply_view();

    generation_shown = false;
    apps_ended_at    = 0;
    leaving          = false;
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    timer = lv_timer_create(tick, TICK_MS, NULL);
    tick(NULL);
    return screen;
}
