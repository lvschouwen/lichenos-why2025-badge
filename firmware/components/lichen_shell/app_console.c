// Console (#150): runs `fw` commands from the badge keyboard, for when there is no computer at hand. Settings > For
// developers > Console. A command line at the bottom, the output above it, Up/Down on an empty line walk the last
// lines typed, Tab moves between the line and the output (Up/Down then scroll it).
//
// A command runs through console_exec, the entry point of POST /console, on a worker task of its own: commands can
// block for seconds and must never hold the LVGL task. The worker posts the output back with lichen_ui_call. One
// command at a time (s_busy). Esc leaves even while one runs: the result carries the generation of the screen that
// started it and is dropped when that screen is gone, so no callback ever reaches a deleted widget.
//
// Safety: the app adds no way around the commands' own confirms (YES, the remote CLI confirm). console_exec marks the
// worker task as not-the-UART (console_from_uart() is false inside it), so `nvs-enc burn` stays USB-only (#116).
#include "app_icons.h"
#include "console_model.h"
#include "lichen_mem.h"
#include "esp_log.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_task.h"
#include "lichen_ui.h"
#include "screen_ids.h"
#include "secure_wipe.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_CAP     16384 // the screen's text: the newest lines of everything run since it opened
#define EXEC_CAP    8192  // what one command may print (console_exec cuts past it with a note)
#define SCROLL_STEP 96
// The UART console runs every command on a 4 KB stack and the Wi-Fi console on the OTA server's 16 KB. This task is
// created per command and freed after it, so the stack costs internal RAM only while a command runs: the UART's 4 KB
// plus margin for console_exec's own frame and the done callback (8 KB after review: a few seconds of internal RAM). Internal, not PSRAM: some commands write flash. The
// high-water mark is logged after each run so a bench pass can confirm the margin.
#define WORKER_STACK 8192

static char const TAG[] = "console_app";

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *status;
    lv_obj_t *out;
    lv_obj_t *label;
    lv_obj_t *ta;
    char     *buf; // PSRAM, OUT_CAP bytes
    size_t    len;
    uint32_t  gen; // told apart from an earlier visit's result
    bool      waiting_old; // the status says Running for a command an earlier visit started
} con_t;

static con_t              *cur;    // the open screen; shell_open keeps it to one (SCREEN_CONSOLE)
static uint32_t            s_gen;
static volatile bool       s_busy; // a worker is alive (set on the LVGL task, cleared by the worker as its last act)
static console_hist_t     *s_hist; // PSRAM, kept while the badge runs: only allow-listed commands, never a YES line (console_model.c)

typedef struct {
    uint32_t gen;
    char    *line;
    char    *exec_buf;
    bool     posted; // the result reached the UI task's queue
} job_t;

typedef struct {
    uint32_t gen;
    char    *text;
    size_t   len;
} reply_t;

static char *psram_alloc(size_t n) {
    return lichen_alloc(NULL, n, LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
}

static void set_status(con_t *c, char const *text, bool amber) {
    lv_label_set_text(c->status, text);
    lv_obj_set_style_text_color(c->status, lv_color_hex(amber ? THEME_AMBER : THEME_DIM), 0);
}

static void show_output(con_t *c, bool to_end) {
    bool empty = c->len == 0;
    lv_label_set_text_static(c->label, empty ? "Type a command, like wifi or help. Enter runs it." : c->buf);
    lv_obj_set_style_text_color(c->label, lv_color_hex(empty ? THEME_DIM : THEME_TEXT), 0);
    if (to_end) {
        lv_obj_update_layout(c->out);
        lv_obj_scroll_by(c->out, 0, -lv_obj_get_scroll_bottom(c->out), LV_ANIM_OFF);
    }
}

static void out_add(con_t *c, char const *text, size_t n) {
    c->len = console_out_append(c->buf, OUT_CAP, c->len, text, n);
}

// ---- the worker ------------------------------------------------------------------------------------------------

static void reply_free(reply_t *r) {
    if (r->text) {
        secure_wipe(r->text, r->len);
        lichen_free(NULL, r->text);
    }
    lichen_free(NULL, r);
}

static void on_reply(void *arg) {
    reply_t *r = arg;
    con_t   *c = cur;
    static char const lost[]    = "(the output could not be shown)\n";
    static char const dropped[] = "(the previous command was still running when the screen closed, so its output is gone; it did run. "
                                  "Run it again to see the output)\n";
    static char const none[]    = "(no output; some commands answer only on the USB console)\n";
    if (c && c->gen == r->gen) {
        // a console_reply_secret line arrives as CONSOLE_SECRET_NOTE (console_exec, #123), never as the secret
        if (!r->text) {
            out_add(c, lost, sizeof(lost) - 1);
        } else if (r->len == 0) {
            out_add(c, none, sizeof(none) - 1);
        } else {
            out_add(c, r->text, r->len);
        }
        set_status(c, "Ready", false);
        show_output(c, true);
    } else if (c && c->waiting_old) { // the screen was reopened while this ran
        c->waiting_old = false;
        out_add(c, dropped, sizeof(dropped) - 1);
        set_status(c, "Ready", false);
        show_output(c, true);
    }
    reply_free(r);
}

static bool post_reply(job_t *j, char const *out, size_t len) {
    reply_t *r = lichen_alloc(NULL, sizeof(*r), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!r) {
        return false;
    }
    r->gen  = j->gen;
    r->text = psram_alloc(len + 1);
    if (r->text) {
        memcpy(r->text, out, len);
        r->len = len;
    }
    if (!lichen_ui_call(on_reply, r)) {
        reply_free(r);
        return false;
    }
    return true;
}

// console_exec calls this once, with its buffer: the reply gets a copy, the worker wipes and frees the buffer
static void exec_done(void *ctx, char const *out, size_t len) {
    job_t *j = ctx;
    j->posted = post_reply(j, out, len);
}

static void worker(void *arg) {
    job_t *j = arg;
    console_exec(j->line, j->exec_buf, EXEC_CAP, exec_done, j);
    // The status says Running until a reply arrives: when the UI was busy or memory short, try again with a note
    static char const lost[] = "(the output could not be shown)\n";
    for (int i = 0; i < 3 && !j->posted; i++) {
        j->posted = post_reply(j, lost, sizeof(lost) - 1);
        if (!j->posted) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    secure_wipe(j->line, CONSOLE_LINE_MAX + 1);
    secure_wipe(j->exec_buf, EXEC_CAP);
    ESP_LOGI(TAG, "command done, stack never below %u bytes free", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    lichen_free(NULL, j->line);
    lichen_free(NULL, j->exec_buf);
    lichen_free(NULL, j);
    s_busy = false; // after the free: the next run may start from the LVGL task at once
}

// LVGL task: starts the worker for line (already trimmed). False when it could not start (nothing is left allocated).
static bool start_job(con_t *c, char const *line) {
    job_t *j = lichen_alloc(NULL, sizeof(*j), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!j) {
        return false;
    }
    j->gen      = c->gen;
    j->line     = psram_alloc(CONSOLE_LINE_MAX + 1);
    j->exec_buf = psram_alloc(EXEC_CAP);
    if (j->line && j->exec_buf) {
        strlcpy(j->line, line, CONSOLE_LINE_MAX + 1);
        s_busy = true;
        if (lichen_task_start(NULL, "console_app", worker, j, WORKER_STACK, LICHEN_PRIO_JOB, NULL)) {
            return true;
        }
        s_busy = false;
    }
    if (j->line) {
        secure_wipe(j->line, CONSOLE_LINE_MAX + 1);
    }
    lichen_free(NULL, j->line);
    lichen_free(NULL, j->exec_buf);
    lichen_free(NULL, j);
    return false;
}

// ---- the screen ------------------------------------------------------------------------------------------------

static void clear_entry(con_t *c) {
    char *text = (char *)lv_textarea_get_text(c->ta); // LVGL's own buffer: wipe it before it is replaced
    secure_wipe(text, strlen(text));
    lv_textarea_set_text(c->ta, "");
}

static void on_ready(lv_event_t *e) {
    con_t *c = lv_event_get_user_data(e);
    char   line[CONSOLE_LINE_MAX + 1];
    strlcpy(line, lv_textarea_get_text(c->ta), sizeof(line));
    char *start = line;
    while (*start == ' ') {
        start++;
    }
    size_t n = strlen(start);
    while (n && start[n - 1] == ' ') {
        start[--n] = '\0';
    }
    if (n == 0) {
        secure_wipe(line, sizeof(line));
        return;
    }
    if (s_busy) { // keep what was typed: it can be run once this one is done
        set_status(c, "Still running the last command", true);
        secure_wipe(line, sizeof(line));
        return;
    }
    bool public = console_line_is_public(start); // an allow-list: anything not known to take no secret is hidden
    console_hist_push(s_hist, start);            // refuses a line that is not public
    char echo[CONSOLE_LINE_MAX + 32];
    char word[24];
    console_first_word(start, word, sizeof(word));
    int m = public ? snprintf(echo, sizeof(echo), "> %s\n", start)
                   : snprintf(echo, sizeof(echo), "> %s (arguments hidden)\n", word);
    out_add(c, echo, (size_t)m);
    clear_entry(c);
    if (start_job(c, start)) {
        set_status(c, "Running...", true);
    } else {
        static char const fail[] = "could not start: out of memory, try again\n";
        out_add(c, fail, sizeof(fail) - 1);
        set_status(c, "Could not run it", true);
    }
    show_output(c, true);
    secure_wipe(line, sizeof(line));
}

static void set_entry(con_t *c, char const *text) {
    clear_entry(c);
    lv_textarea_set_text(c->ta, text);
}

// Up/Down on the line: the history, but only while the line is empty or shows a history line (typed text is not lost)
static void on_entry_key(lv_event_t *e) {
    con_t   *c   = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_UP && key != LV_KEY_DOWN) {
        console_hist_end_browse(s_hist); // an edit: the line is the user's again
        return;
    }
    bool        empty = lv_textarea_get_text(c->ta)[0] == '\0';
    char const *line  = NULL;
    if (empty || console_hist_browsing(s_hist)) {
        line = key == LV_KEY_UP ? console_hist_older(s_hist) : console_hist_newer(s_hist);
    }
    if (line) {
        set_entry(c, line);
    }
}

static void on_out_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_target(e), key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
    }
}

static void on_delete(lv_event_t *e) {
    con_t *c = lv_event_get_user_data(e);
    lv_label_set_text_static(c->label, ""); // the label must not point into the buffer freed next
    secure_wipe(c->buf, OUT_CAP);
    lichen_free(NULL, c->buf);
    cur = NULL;
    lichen_free(NULL, c);
}

static void focus_style(lv_obj_t *obj) {
    lv_obj_set_style_border_width(obj, 3, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(THEME_AMBER), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(obj, lv_color_hex(THEME_AMBER), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_ANY);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

static void build_output(con_t *c, lv_obj_t *screen) {
    c->out = lv_obj_create(screen);
    lv_obj_set_width(c->out, LV_PCT(100));
    lv_obj_set_flex_grow(c->out, 1);
    focus_style(c->out);
    lv_obj_set_style_pad_all(c->out, 14, 0);
    lv_obj_set_scrollbar_mode(c->out, LV_SCROLLBAR_MODE_OFF);
    shell_scroll_tame(c->out);
    shell_no_arrow_scroll(c->out); // Up/Down scroll by on_out_key, not by LVGL's own jump
    c->label = lichen_label_wrap(c->out, LV_PCT(100));
    lv_obj_set_style_text_font(c->label, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_add_event_cb(c->out, on_out_key, LV_EVENT_KEY, NULL);
}

static void build_entry(con_t *c, lv_obj_t *screen) {
    c->ta = lv_textarea_create(screen);
    lv_obj_set_width(c->ta, LV_PCT(100));
    lv_textarea_set_one_line(c->ta, true);
    lv_textarea_set_max_length(c->ta, CONSOLE_LINE_MAX);
    lv_textarea_set_placeholder_text(c->ta, "fw command");
    focus_style(c->ta);
    lv_obj_set_style_text_color(c->ta, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(c->ta, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_style_pad_all(c->ta, 14, 0);
    lv_obj_set_style_bg_opa(c->ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(c->ta, lv_color_hex(THEME_AMBER), LV_PART_CURSOR);
    lv_obj_set_style_text_color(c->ta, lv_color_hex(THEME_BG), LV_PART_CURSOR);
    lv_obj_add_event_cb(c->ta, on_ready, LV_EVENT_READY, c);
    lv_obj_add_event_cb(c->ta, on_entry_key, LV_EVENT_KEY, c);
}

static lv_obj_t *build_console(void) {
    lv_obj_t *screen = shell_page_create();
    con_t    *c      = lichen_alloc(NULL, sizeof(*c), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!c) {
        lv_label_set_text(lv_label_create(screen), "Out of memory");
        return screen;
    }
    c->buf = psram_alloc(OUT_CAP);
    if (!s_hist) {
        s_hist = (console_hist_t *)psram_alloc(sizeof(*s_hist));
        if (s_hist) {
            console_hist_init(s_hist);
        }
    }
    if (!c->buf || !s_hist) {
        lichen_free(NULL, c->buf);
        lichen_free(NULL, c);
        lv_label_set_text(lv_label_create(screen), "Out of memory");
        return screen;
    }
    c->screen = screen;
    c->gen    = ++s_gen;
    console_hist_end_browse(s_hist);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(screen, false);
    lichen_heading(screen, NULL, "Console");
    c->status = lichen_label_line(screen, LV_PCT(100));
    build_output(c, screen);
    build_entry(c, screen);
    lv_group_t *group = lv_group_get_default();
    if (lv_obj_get_group(c->out) != group) {
        lv_group_add_obj(group, c->out); // after the line: it takes the focus first, Tab reaches the output
    }
    lv_group_focus_obj(c->ta);
    c->waiting_old = s_busy;
    set_status(c, s_busy ? "Running..." : "Ready", s_busy);
    show_output(c, false);
    shell_page_hint(screen, "Enter", "run", "Tab", "scroll", NULL);
    cur = c;
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, c);
    return screen;
}

void console_open_from_settings(void *arg) {
    (void)arg;
    shell_open(build_console, SCREEN_CONSOLE);
}
