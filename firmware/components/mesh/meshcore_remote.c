// Log in, Status and CLI screens for a repeater or room (#51): see meshcore_remote.h. Everything with an lv_obj_t runs
// in the LVGL task; MeshCore is only reached through mesh_remote_start / mesh_remote_get, which never wait for it.
#include "meshcore_remote.h"

#include "esp_heap_caps.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_rset.h"
#include "meshcore_run.h"
#include "text_entry.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define SCREEN_LOGIN  163
#define SCREEN_STATUS 164
#define SCREEN_CLI    165
#define POLL_MS       250
#define CLI_TEXT_MAX  (RR_CLI_KEEP * (RR_CMD_MAX + RR_REPLY_MAX + 8) + 16)

typedef struct {
    rr_t      rr;
    rr_line_t lines[RR_LINES_MAX];
    char      text[CLI_TEXT_MAX];
} view_mem_t;

static uint8_t    target[6];
static char       title[MESH_NAME_MAX + 1];
static view_mem_t *mem;       // PSRAM, while one of the screens exists
static lv_obj_t   *screen_obj; // the screen that owns mem and timer
static lv_timer_t *timer;
static lv_obj_t   *result, *again, *note;
static uint32_t    seq; // the request this screen made; 0 when it could not start
static bool        shown_busy;

static void wipe_pw(void);

static void set_text(lv_obj_t *label, char const *text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text); // only on a change: the same text every tick would redraw for nothing
    }
}

static lv_obj_t *add_text(lv_obj_t *menu, char const *text) {
    lv_obj_t *label = menu_note(menu, text);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_TEXT), 0);
    return label;
}

static void set_target(uint8_t const prefix[6], char const *name) {
    memcpy(target, prefix, sizeof(target));
    snprintf(title, sizeof(title), "%s", name[0] ? name : "(no name)");
}

static void on_delete(lv_event_t *e) {
    if (lv_event_get_target(e) != screen_obj) {
        return; // a late delete of an older screen must not free what a newer one owns
    }
    screen_obj = NULL;
    wipe_pw(); // leaving before the answer: the password is not kept
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    heap_caps_free(mem);
    mem    = NULL;
    result = again = note = NULL;
}

// The screen, its PSRAM memory and its poll timer; NULL (the menu says why) when there is no memory
static lv_obj_t *build_common(char const *heading, lv_timer_cb_t tick, lv_obj_t **menu_out) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create_named(screen, heading, title);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    screen_obj = screen;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    heap_caps_free(mem);
    mem        = heap_caps_calloc(1, sizeof(*mem), MALLOC_CAP_SPIRAM);
    seq        = 0;
    shown_busy = false;
    *menu_out  = menu;
    if (!mem) {
        menu_note(menu, "Not enough memory.");
        return screen;
    }
    timer = lv_timer_create(tick, POLL_MS, NULL);
    return screen;
}

// ---- Log in ----------------------------------------------------------------------------------------------------------

static char pw_hold[MF_PASSWORD_MAX + 1]; // until the answer: remembered when the repeater accepts it, else wiped
static bool pw_pending;

static void wipe_pw(void) {
    volatile char *v = pw_hold;
    for (size_t i = 0; i < sizeof(pw_hold); i++) {
        v[i] = 0;
    }
    pw_pending = false;
}

static void login_tick(lv_timer_t *t) {
    (void)t;
    if (!mem || !result || !seq || !mesh_remote_get(&mem->rr)) {
        return;
    }
    rr_t const *r = &mem->rr;
    if (r->seq != seq) {
        set_text(result, "Another request to a repeater or room was started.");
        wipe_pw();
        return;
    }
    char text[128];
    rr_state_text(r, text, sizeof(text));
    if (r->state == RR_DONE && pw_pending) {
        bool saved = meshcore_pw_set(target, pw_hold);
        wipe_pw();
        strlcat(text, saved ? ". Password remembered." : ". The password could not be saved.", sizeof(text));
    } else if (!rr_busy(r)) {
        wipe_pw(); // refused or no answer: not kept
    }
    set_text(result, text);
}

static void login_start(void) {
    char const *why = NULL;
    seq             = mesh_remote_start(target, RR_LOGIN, pw_hold, &why);
    if (!seq) {
        char text[96];
        snprintf(text, sizeof(text), "Not started: %s", why ? why : "?");
        set_text(result, text);
        wipe_pw();
        return;
    }
    login_tick(NULL);
}

static lv_obj_t *build_login(void) {
    lv_obj_t *menu;
    lv_obj_t *screen = build_common("Log in", login_tick, &menu);
    if (!mem) {
        wipe_pw();
        return screen;
    }
    menu_note(menu, "Sent now, along the stored path or flooded. A repeater or room answers only with the right password. "
                    "After a login the badge keeps the session alive on air until you log out.");
    result = add_text(menu, "Sending...");
    login_start();
    return screen;
}

static void login_entered(char const *password, void *arg) {
    (void)arg;
    snprintf(pw_hold, sizeof(pw_hold), "%s", password);
    pw_pending = true;
    shell_open(build_login, SCREEN_LOGIN); // the Enter that closed the entry is the person's go-ahead
}

void meshcore_remote_login(uint8_t const prefix[6], char const *name) {
    set_target(prefix, name);
    char heading[MESH_NAME_MAX + 24], saved[MF_PASSWORD_MAX + 2];
    snprintf(heading, sizeof(heading), "Password for\n%s", title);
    saved[0] = 0;
    meshcore_pw_get(target, saved, sizeof(saved));
    text_entry_open(heading, saved, true, MF_PASSWORD_MAX, login_entered, NULL);
    memset(saved, 0, sizeof(saved));
}

// ---- Status -----------------------------------------------------------------------------------------------------------

static void status_show(void) {
    if (!mem || !result || !seq || !mesh_remote_get(&mem->rr)) {
        return;
    }
    rr_t const *r = &mem->rr;
    if (r->seq != seq) {
        set_text(result, "Another request was started: Ask again.");
        return;
    }
    char text[128];
    rr_state_text(r, text, sizeof(text));
    size_t at = 0;
    mem->text[0] = 0;
    if (r->have_status && memcmp(r->status_prefix, target, 6) == 0) {
        size_t n = rr_status_lines(&r->status, mem->lines, RR_LINES_MAX);
        for (size_t i = 0; i < n && at + 1 < sizeof(mem->text); i++) {
            at += (size_t)snprintf(mem->text + at, sizeof(mem->text) - at, "%s%s", i ? "\n" : "", mem->lines[i].text);
        }
    }
    if (r->state != RR_DONE) {
        snprintf(mem->text + at, sizeof(mem->text) - at, "%s%s", at ? "\n\n(last answer above) " : "", text);
    }
    set_text(result, mem->text);
    bool busy = rr_busy(r);
    if (busy != shown_busy) {
        menu_set_value(again, busy ? "..." : "");
        shown_busy = busy;
    }
}

static void status_tick(lv_timer_t *t) {
    (void)t;
    status_show();
}

static void status_start(void *arg) {
    (void)arg;
    if (mem && seq && mesh_remote_get(&mem->rr) && mem->rr.seq == seq && rr_busy(&mem->rr)) {
        return; // still waiting for this one: a held Enter does not queue more
    }
    char const *why = NULL;
    seq             = mesh_remote_start(target, RR_STATUS, NULL, &why);
    if (!seq) {
        char text[96];
        snprintf(text, sizeof(text), "Not started: %s", why ? why : "?");
        set_text(result, text);
        return;
    }
    status_show();
}

static lv_obj_t *build_status(void) {
    lv_obj_t *menu;
    lv_obj_t *screen = build_common("Status", status_tick, &menu);
    if (!mem) {
        return screen;
    }
    again = menu_add(menu, "Ask again", "", status_start, NULL);
    mem->rr.seq = 0;
    if (mesh_remote_get(&mem->rr) && !rr_logged_in(&mem->rr, target)) {
        menu_note(menu, "Not logged in from here: a repeater answers only logged-in clients.");
    }
    result = add_text(menu, "Asking...");
    status_start(NULL);
    return screen;
}

void meshcore_remote_status(uint8_t const prefix[6], char const *name) {
    set_target(prefix, name);
    shell_open(build_status, SCREEN_STATUS);
}

// ---- CLI --------------------------------------------------------------------------------------------------------------

static void cli_show(void) {
    if (!mem || !result || !mesh_remote_get(&mem->rr)) {
        return;
    }
    rr_t const *r = &mem->rr;
    if (r->cli_n == 0 || memcmp(r->cli_prefix, target, 6) != 0) {
        set_text(result, "No commands yet: Command... types one, it is sent when you press Enter.");
    } else {
        rr_cli_format(r, mem->text, sizeof(mem->text));
        set_text(result, mem->text);
    }
    bool busy = rr_busy(r);
    if (busy != shown_busy) {
        menu_set_value(again, busy ? "..." : "");
        shown_busy = busy;
    }
}

static void cli_tick(lv_timer_t *t) {
    (void)t;
    cli_show();
}

static void cli_send(char const *command) {
    char const *why = NULL;
    if (mesh_remote_start(target, RR_CLI, command, &why)) {
        set_text(note, "");
        cli_show();
    } else {
        char text[96];
        snprintf(text, sizeof(text), "Not sent: %s", why ? why : "?");
        set_text(note, text);
    }
}

// A radio change or a reboot typed on the free line asks first, as on the Settings page (#117); erase never goes out
// (mesh_remote.c refuses it)
static char cli_pending[RR_CMD_MAX + 1];

static void cli_answer(bool yes) {
    if (yes && mem && note) {
        cli_send(cli_pending);
    } else if (mem && note) {
        set_text(note, "Not sent.");
    }
    memset(cli_pending, 0, sizeof(cli_pending));
}

static void cli_ask(char const *command, bool radio) {
    char t[160];
    snprintf(cli_pending, sizeof(cli_pending), "%s", command);
    snprintf(t, sizeof(t), "Send \"%s\" to %s?", command, title);
    shell_dialog_t const d = {
        .title        = t,
        .count_prefix = radio ? "A wrong radio cuts it off the mesh; only a visit to it undoes that. ("
                              : "It restarts or stops the repeater. (", // only the sentence before "(" is used
        .seconds      = 30,
        .careful      = true,
        .confirm_label = "send",
    };
    if (!shell_dialog_countdown(&d, cli_answer)) {
        set_text(note, mc_busy_note());
        memset(cli_pending, 0, sizeof(cli_pending));
    }
}

static void cli_entered(char const *command, void *arg) {
    (void)arg;
    if (!*command || !mem || !note) { // nothing typed, or the screen went away meanwhile
        return;
    }
    rs_cmd_class_t cls = rs_cmd_class(command);
    if (cls == RS_CMD_RADIO || cls == RS_CMD_REBOOT) {
        cli_ask(command, cls == RS_CMD_RADIO);
        return;
    }
    cli_send(command);
}

static void cli_command(void *arg) {
    (void)arg;
    char heading[MESH_NAME_MAX + 24];
    snprintf(heading, sizeof(heading), "Command for\n%s", title);
    text_entry_open(heading, "", false, RR_CMD_MAX, cli_entered, NULL);
}

static lv_obj_t *build_cli(void) {
    lv_obj_t *menu;
    lv_obj_t *screen = build_common("Commands", cli_tick, &menu);
    if (!mem) {
        return screen;
    }
    again = menu_add(menu, "Command...", "", cli_command, NULL);
    menu_note(menu, "Needs a login with admin rights. The newest reply is on top; Down scrolls.");
    note   = menu_note(menu, "");
    result = add_text(menu, "");
    cli_show();
    return screen;
}

void meshcore_remote_cli(uint8_t const prefix[6], char const *name) {
    set_target(prefix, name);
    shell_open(build_cli, SCREEN_CLI);
}
