// A repeater's Settings page (#117), opened from its details (meshcore_contact.c) after a login as admin. The values are
// read over the CLI one get at a time (mesh_rset_run.h, stepped by this screen's LVGL timer, so every LVGL call stays on
// the LVGL task); a row changes only on an Enter on it and a second Enter in its edit field, and the radio and a reboot
// only after a careful confirm. erase is never sent (mesh_remote.c refuses it).
#include "meshcore_remote.h"

#include "esp_heap_caps.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_rset_run.h"
#include "meshcore_run.h"
#include "text_entry.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_RSET 166
#define TICK_MS     250
#define ASK_S       30 // the careful dialogs cancel by themselves after this
#define SHOWN_MAX   (RS_VALUE_MAX + 8)

typedef struct {
    rs_run_t run;
    char     shown[RS_ROWS][SHOWN_MAX]; // what each row shows: redrawn only on a change
} rset_mem_t;

static uint8_t     target[6];
static char        title[MESH_NAME_MAX + 1];
static rset_mem_t *mem; // PSRAM, while the screen exists
static lv_obj_t   *screen_obj, *note, *again_row, *rows[RS_ROWS];
static lv_timer_t *timer;
static bool        shown_busy;
static char        pending[RS_CMD_MAX + 1];         // the radio value between its entry and its confirm
static char        pw_first[MF_PASSWORD_MAX + 1];  // the new password between its two entries
static char        pw_hold[MF_PASSWORD_MAX + 1];   // the new password until the repeater took it
static bool        pw_wait;

static void wipe(void *p, size_t n) {
    volatile char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

static void wipe_secrets(void) {
    wipe(pw_first, sizeof(pw_first));
    wipe(pw_hold, sizeof(pw_hold));
    pw_wait = false;
}

static void set_note(char const *text, bool error) {
    if (note) {
        lv_obj_set_style_text_color(note, lv_color_hex(error ? THEME_RED : THEME_TEAL), 0);
        if (strcmp(lv_label_get_text(note), text) != 0) {
            lv_label_set_text(note, text);
        }
    }
}

static bool admin_here(void) {
    rr_t *rr = mem ? &mem->run.rr : NULL;
    return rr && mesh_remote_get(rr) && rr_logged_in(rr, target) && rr->admin;
}

static bool set_waiting(void) {
    rs_t const *s = &mem->run.rs;
    return s->set_row >= 0 || (s->cur_row >= 0 && s->cur_set);
}

// The new password is remembered once the repeater confirmed it, and only then (#117)
static void settle_password(void) {
    rs_t const *s  = &mem->run.rs;
    int         pw = rs_row_find("password");
    if (!pw_wait || set_waiting() || s->last_row != pw) {
        return;
    }
    if (s->last == RS_R_OK) {
        bool saved = meshcore_pw_set(target, pw_hold);
        set_note(saved ? "Password changed, and remembered on this badge." : "Password changed; this badge could not "
                                                                             "save it (Log in asks for it).",
                 !saved);
    }
    wipe_secrets();
}

static void show_outcome(void) {
    rs_t const *s = &mem->run.rs;
    char        text[RS_TEXT_MAX + 64];
    size_t      done, total;
    rs_progress(s, &done, &total);
    if (set_waiting()) {
        set_note("Sending the change...", false);
    } else if (rs_busy(s)) {
        snprintf(text, sizeof(text), "Reading %u of %u...", (unsigned)(done + 1 > total ? total : done + 1),
                 (unsigned)total);
        set_note(text, false);
    } else if (s->last_row >= 0) {
        bool radio = rs_row_at((size_t)s->last_row)->kind == RS_RADIO && s->last == RS_R_OK;
        snprintf(text, sizeof(text), "%s: %s%s", rs_row_at((size_t)s->last_row)->label, s->last_text,
                 radio ? ". The repeater changes over after a reboot." : "");
        set_note(text, s->last != RS_R_OK);
    } else if (mem->run.why) {
        snprintf(text, sizeof(text), "Read %u rows. Last problem: %s", (unsigned)total, mem->run.why);
        set_note(text, true);
    } else {
        set_note("Enter on a row changes it; nothing is sent before the Enter in its field.", false);
    }
}

static void refresh(void) {
    char text[SHOWN_MAX];
    for (size_t i = 0; i < RS_ROWS; i++) {
        rs_value_text(&mem->run.rs, i, text, sizeof(text));
        if (rows[i] && strcmp(text, mem->shown[i]) != 0) {
            menu_set_value(rows[i], text);
            snprintf(mem->shown[i], SHOWN_MAX, "%s", text);
        }
    }
    bool busy = rs_run_busy(&mem->run);
    if (busy != shown_busy) {
        menu_set_value(again_row, busy ? "..." : "");
        shown_busy = busy;
    }
    show_outcome();
}

static void tick(lv_timer_t *t) {
    (void)t;
    if (!mem) {
        return;
    }
    if (rs_run_step(&mem->run)) {
        refresh();
        settle_password();
    }
}

static void read_all(void *arg) {
    (void)arg;
    if (!mem) {
        return;
    }
    if (!admin_here()) {
        set_note("Log in as admin first (Log in on the repeater's details).", true);
        return;
    }
    mem->run.why = NULL;
    rs_read_all(&mem->run.rs);
    refresh();
}

static void queue(size_t i, char const *value) {
    if (!mem) {
        return; // the screen went away meanwhile
    }
    mem->run.why    = NULL;
    char const *why = rs_queue_set(&mem->run.rs, i, value);
    if (why) {
        char text[96];
        snprintf(text, sizeof(text), "Not sent: %s", why);
        set_note(text, true);
        return;
    }
    refresh();
}

// ---- the careful ones -------------------------------------------------------------------------------------------------

static void radio_answer(bool yes) {
    if (yes && mem) {
        queue((size_t)rs_row_find("radio"), pending);
    } else if (mem) {
        set_note("Radio unchanged: nothing sent.", false);
    }
    pending[0] = 0;
}

// The badge's own radio: the repeater set to anything else is lost to this badge once it reboots
static bool same_as_badge(char const *value) {
    mesh_settings_t st;
    float           f, bw;
    uint8_t         sf, cr;
    return mesh_get_settings(&st) && rs_radio_parse(value, &f, &bw, &sf, &cr) && fabsf(f - st.freq) < 0.0005f &&
           fabsf(bw - st.bw) < 0.001f && sf == st.sf && cr == st.cr;
}

static void ask_radio(char const *value) {
    char t[96];
    bool same = same_as_badge(value);
    snprintf(pending, sizeof(pending), "%s", value);
    snprintf(t, sizeof(t), "Set the radio of %s to %s?", title, value);
    shell_dialog_t const d = {
        .title        = t,
        .count_prefix = same ? "Same as this badge. A wrong radio needs a visit to the repeater to undo. (cancels in "
                             : "NOT this badge's radio: it loses the repeater. Only a visit undoes it. (cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: set it \xC2\xB7 Esc: keep the old one",
        .seconds      = ASK_S,
        .careful      = true,
    };
    if (!shell_dialog_countdown(&d, radio_answer)) {
        set_note("Could not ask, try again", true);
    }
}

static void reboot_answer(bool yes) {
    if (yes && mem) {
        queue((size_t)rs_row_find("reboot"), "");
    }
}

static void ask_reboot(void) {
    char t[96];
    snprintf(t, sizeof(t), "Reboot %s? It is off the air for a while; log in again after.", title);
    shell_dialog_t const d = {
        .title        = t,
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: reboot \xC2\xB7 Esc: keep it running",
        .seconds      = ASK_S,
        .careful      = true,
    };
    if (!shell_dialog_countdown(&d, reboot_answer)) {
        set_note("Could not ask, try again", true);
    }
}

static void pw_second(char const *text, void *arg) {
    (void)arg;
    bool same = strcmp(text, pw_first) == 0;
    if (!mem || !same) {
        wipe_secrets();
        set_note("The two passwords differ: nothing sent.", true);
        return;
    }
    snprintf(pw_hold, sizeof(pw_hold), "%s", pw_first);
    wipe(pw_first, sizeof(pw_first));
    pw_wait = true;
    queue((size_t)rs_row_find("password"), pw_hold);
    if (!set_waiting()) {
        wipe_secrets(); // refused here (rs_queue_set said why)
    }
}

static void pw_entered(char const *text, void *arg) {
    (void)arg;
    char const *why = rs_check((size_t)rs_row_find("password"), text);
    if (why) {
        char t[80];
        snprintf(t, sizeof(t), "Not sent: %s", why);
        set_note(t, true);
        return;
    }
    snprintf(pw_first, sizeof(pw_first), "%s", text);
    text_entry_open("The new password again", "", true, MF_PASSWORD_MAX, pw_second, NULL);
}

// ---- rows -------------------------------------------------------------------------------------------------------------

static void value_entered(char const *text, void *arg) {
    size_t i = (size_t)(uintptr_t)arg;
    if (!mem) {
        return;
    }
    char now[RS_VALUE_MAX];
    rs_edit_text(&mem->run.rs, i, now, sizeof(now));
    if (strcmp(text, now) == 0) {
        set_note("Unchanged: nothing sent.", false);
        return;
    }
    char const *why = rs_check(i, text);
    if (why) {
        char t[96];
        snprintf(t, sizeof(t), "Not sent: %s", why);
        set_note(t, true);
        return;
    }
    if (rs_row_at(i)->kind == RS_RADIO) {
        ask_radio(text);
    } else {
        queue(i, text);
    }
}

static void hint_for(rs_row_t const *row, char *out, size_t cap) {
    switch (row->kind) {
    case RS_INT:
    case RS_DEC:
        snprintf(out, cap, "%s%g to %g%s%s", row->zero_ok ? "0 = off, " : "", (double)row->min, (double)row->max,
                 row->unit ? " " : "", row->unit ? row->unit : "");
        break;
    case RS_ONOFF: snprintf(out, cap, "on or off"); break;
    case RS_CHOICE: snprintf(out, cap, "off, minimal, moderate, strict"); break;
    case RS_RADIO: snprintf(out, cap, "MHz,kHz,SF,CR"); break;
    default: snprintf(out, cap, "up to 31 characters"); break;
    }
}

static void on_row(void *arg) {
    size_t          i   = (size_t)(uintptr_t)arg;
    rs_row_t const *row = rs_row_at(i);
    if (!mem || !row) {
        return;
    }
    if (!admin_here()) {
        set_note("Log in as admin first (Log in on the repeater's details).", true);
        return;
    }
    if (!rs_row_editable(i)) {
        set_note("Read only.", false);
        return;
    }
    if (set_waiting()) {
        set_note("Wait for the answer to the last change.", true);
        return;
    }
    uint8_t st = mem->run.rs.st[i];
    if (rs_row_readable(i) && st != RS_V_VALUE) {
        set_note(st == RS_V_NONE ? "The repeater did not answer this one: Read again first." : "Still reading it.", true);
        return;
    }
    if (row->kind == RS_REBOOT) {
        ask_reboot();
        return;
    }
    char heading[MESH_NAME_MAX + 64];
    if (row->kind == RS_PASSWORD) {
        snprintf(heading, sizeof(heading), "New admin password for %s", title);
        text_entry_open(heading, "", true, MF_PASSWORD_MAX, pw_entered, NULL);
        return;
    }
    char hint[48], now[RS_VALUE_MAX];
    hint_for(row, hint, sizeof(hint));
    rs_edit_text(&mem->run.rs, i, now, sizeof(now));
    snprintf(heading, sizeof(heading), "%s (%s)", row->label, hint);
    text_entry_open(heading, now, false, RS_VALUE_MAX - 1, value_entered, arg);
}

// ---- the screen -------------------------------------------------------------------------------------------------------

static void on_delete(lv_event_t *e) {
    if (lv_event_get_target(e) != screen_obj) {
        return;
    }
    screen_obj = NULL;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    wipe_secrets(); // an answer that comes after this is not waited for: the password is then not remembered
    pending[0] = 0;
    heap_caps_free(mem);
    mem       = NULL;
    note      = again_row = NULL;
    memset(rows, 0, sizeof(rows));
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    char      head[MESH_NAME_MAX + 16];
    snprintf(head, sizeof(head), "Settings: %s", title);
    lv_obj_t *menu = menu_create(screen, head);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    screen_obj = screen;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    heap_caps_free(mem);
    memset(rows, 0, sizeof(rows));
    wipe_secrets();
    mem = heap_caps_calloc(1, sizeof(*mem), MALLOC_CAP_SPIRAM);
    if (!mem) {
        menu_note(menu, "Not enough memory.");
        return screen;
    }
    rs_run_init(&mem->run, target);
    shown_busy = false;
    again_row  = menu_add(menu, "Read again", "", read_all, NULL);
    note       = menu_note(menu, "");
    for (size_t i = 0; i < RS_ROWS; i++) {
        rs_row_t const *row = rs_row_at(i);
        if (row->kind == RS_PASSWORD) {
            menu_note(menu, "Careful: a wrong radio cuts the repeater off the mesh, and only someone at the repeater "
                            "can undo it. erase is never sent from the badge.");
        }
        rows[i] = menu_add(menu, row->label, "", on_row, (void *)(uintptr_t)i);
    }
    menu_note(menu, "Esc: back");
    read_all(NULL); // the values are read as the page opens (gets only), as the Status screen asks as it opens
    timer = lv_timer_create(tick, TICK_MS, NULL);
    return screen;
}

void meshcore_remote_settings(uint8_t const prefix[6], char const *name) {
    memcpy(target, prefix, sizeof(target));
    snprintf(title, sizeof(title), "%s", name[0] ? name : "(no name)");
    shell_open(build, SCREEN_RSET);
}
