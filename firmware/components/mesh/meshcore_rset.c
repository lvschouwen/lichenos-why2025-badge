// A repeater's Settings page (#117), opened from its details (meshcore_contact.c) after a login as admin. The values are
// read over the CLI one get at a time (mesh_rset_run.h, stepped by this screen's LVGL timer, so every LVGL call stays on
// the LVGL task); a row changes only on an Enter on it and a second Enter in its edit field, and the radio and a restart
// only after a careful confirm. erase is never sent (mesh_remote.c refuses it).
#include "meshcore_remote.h"
#include "screen_ids.h"

#include "lichen_mem.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_rset_run.h"
#include "meshcore_run.h"
#include "text_entry.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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
static lv_obj_t   *screen_obj, *menu_obj, *note, *again_row, *rows[RS_ROWS];
static lv_timer_t *timer;
static bool        shown_busy;
static bool        reading;      // a read of all rows is under way: the menu's status line follows it and must end (R5)
static int         shown_done;   // the count the status line shows
static char        pending[RS_CMD_MAX + 1];         // the radio value between its entry and its confirm
static char        pw_first[MF_PASSWORD_MAX + 1];  // the new password between its two entries
static char        pw_hold[MF_PASSWORD_MAX + 1];   // the new password until the repeater took it
static bool        pw_wait;
static char        local_text[160]; // the note of the last action: show_outcome() owns the label and redraws it every tick
static bool        local_err, local_on;

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

// A one-off note from an action: kept (and shown by show_outcome) until the next action or the change on its way
static void set_local(char const *text, bool error) {
    snprintf(local_text, sizeof(local_text), "%s", text);
    local_err = error;
    local_on  = true;
    set_note(text, error);
}

static void clear_local(void) {
    local_on = false;
}

static bool admin_here(void) {
    return mem && rs_run_admin(&mem->run);
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
        set_local(saved ? "Password changed, and remembered on this badge." : "Password changed; this badge could not "
                                                                             "save it (Log in asks for it).",
                 !saved);
    }
    wipe_secrets();
}

// How many of the readable rows have a value
static size_t answered_rows(rs_t const *s) {
    size_t n = 0;
    for (size_t i = 0; i < RS_ROWS; i++) {
        n += rs_row_readable(i) && s->st[i] == RS_V_VALUE;
    }
    return n;
}

// The read's status line: "Reading 3 of 17" while it runs, then it ends in "Read 17 of 17" or "Read 14 of 17, 3 did not
// answer" (R5). Called on every tick, so it ends however the last answer came in.
static void show_read_status(void) {
    rs_t const *s = &mem->run.rs;
    if (!reading || set_waiting()) {
        return;
    }
    size_t done, total;
    rs_progress(s, &done, &total);
    if (rs_run_busy(&mem->run)) {
        if ((int)done != shown_done) {
            menu_status_progress(menu_obj, (int)done, (int)total);
            shown_done = (int)done;
        }
        return;
    }
    reading         = false;
    clear_local(); // a "Still reading it." must not hide the read's own "Last problem"
    size_t got      = answered_rows(s);
    char   text[64];
    if (got == total) {
        snprintf(text, sizeof(text), "Read %u of %u", (unsigned)got, (unsigned)total);
        menu_status_done(menu_obj, text);
    } else {
        snprintf(text, sizeof(text), "Read %u of %u, %u did not answer", (unsigned)got, (unsigned)total,
                 (unsigned)(total - got));
        menu_status_fail(menu_obj, text, s->stopped ? "Out of reach? Read again tries again" : "Read again tries again");
    }
}

// The line under Read again: the outcome of the last change, or why a read had a problem
static void show_outcome(void) {
    rs_t const *s = &mem->run.rs;
    char        text[RS_TEXT_MAX + 64], words[RS_TEXT_MAX];
    bool        waiting = set_waiting();
    bool        problem = mem->run.why && !rs_run_busy(&mem->run);
    if (waiting) {
        local_on = false; // the answer to the change is the newer news
    }
    switch (rs_note_pick(waiting, local_on, s->last_row >= 0, problem)) {
    case RS_NOTE_SENDING: set_note("Sending the change...", false); break;
    case RS_NOTE_LOCAL: set_note(local_text, local_err); break;
    case RS_NOTE_OUTCOME:
        rs_outcome_words((size_t)s->last_row, (rs_reply_t)s->last, s->last_text, words, sizeof(words));
        snprintf(text, sizeof(text), "%s: %s", rs_row_at((size_t)s->last_row)->label, words);
        set_note(text, s->last != RS_R_OK);
        break;
    case RS_NOTE_PROBLEM:
        snprintf(text, sizeof(text), "Last problem: %s", mem->run.why);
        set_note(text, true);
        break;
    default: set_note("", false); break;
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
        menu_set_value(again_row, busy ? "Reading..." : "");
        shown_busy = busy;
    }
    show_read_status();
    show_outcome();
}

static void tick(lv_timer_t *t) {
    (void)t;
    if (!mem) {
        return;
    }
    rs_run_step(&mem->run);
    // Not only when the step says something changed: the last answer must always reach the screen, so the read ends
    // in a final line (R5). refresh() redraws only what differs, so a quiet tick costs a few comparisons.
    refresh();
    settle_password();
}

static void read_all(void *arg) {
    (void)arg;
    if (!mem) {
        return;
    }
    if (!admin_here()) {
        set_local("Log in as admin first (Log in on the repeater's details).", true);
        return;
    }
    if (reading) {
        return; // still reading: a held Enter does not begin it again
    }
    clear_local();
    mem->run.why = NULL;
    rs_read_all(&mem->run.rs);
    reading    = true;
    shown_done = -1;
    menu_status_begin(menu_obj, "Reading");
    refresh();
}

static void queue(size_t i, char const *value) {
    if (!mem) {
        return; // the screen went away meanwhile
    }
    clear_local();
    mem->run.why    = NULL;
    char const *why = rs_queue_set(&mem->run.rs, i, value);
    if (why) {
        char text[96];
        snprintf(text, sizeof(text), "Not sent: %s", why);
        set_local(text, true);
        return;
    }
    refresh();
}

// ---- the careful ones -------------------------------------------------------------------------------------------------

static void radio_answer(bool yes) {
    if (yes && mem && !admin_here()) {
        set_local("No longer logged in as admin: nothing sent.", true); // the login may have ended during the dialog
    } else if (yes && mem) {
        queue((size_t)rs_row_find("radio"), pending);
    } else if (mem) {
        set_local("Radio unchanged: nothing sent.", false);
    }
    pending[0] = 0;
}

// The badge's own radio: the repeater set to anything else is lost to this badge once it restarts
static bool same_as_badge(char const *value) {
    mesh_settings_t st;
    float           f, bw;
    uint8_t         sf, cr;
    return mesh_get_settings(&st) && rs_radio_parse(value, &f, &bw, &sf, &cr) && fabsf(f - st.freq) < 0.0005f &&
           fabsf(bw - st.bw) < 0.001f && sf == st.sf && cr == st.cr;
}

static void ask_radio(char const *value) {
    char t[160], human[RS_VALUE_MAX];
    bool same = same_as_badge(value);
    snprintf(pending, sizeof(pending), "%s", value);
    rs_format_value((size_t)rs_row_find("radio"), value, human, sizeof(human));
    snprintf(t, sizeof(t), "Set the radio of %s to %s?", title, human);
    shell_dialog_t const d = {
        .title         = t,
        .count_prefix  = same ? "Same as this badge. A wrong radio needs a visit to the repeater to undo. ("
                              : "This is not the badge's radio: the badge loses the repeater. Only a visit undoes it. (",
        .seconds       = ASK_S,
        .careful       = true,
        .confirm_label = "set radio",
    };
    if (!shell_dialog_countdown(&d, radio_answer)) {
        set_local(mc_busy_note(), true);
    }
}

// The Restart row is a DANGER row: the careful dialog came before this
static void restart_go(void *arg) {
    (void)arg;
    if (mem && !admin_here()) {
        set_local("No longer logged in as admin: nothing sent.", true);
    } else if (mem) {
        queue((size_t)rs_row_find("reboot"), "");
    }
}

static void pw_second(char const *text, void *arg) {
    (void)arg;
    bool same = strcmp(text, pw_first) == 0;
    if (!mem || !same) {
        wipe_secrets();
        set_local("The two passwords differ: nothing sent.", true);
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
        set_local(t, true);
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
        set_local("Unchanged: nothing sent.", false);
        return;
    }
    char const *why = rs_check(i, text);
    if (why) {
        char t[96];
        snprintf(t, sizeof(t), "Not sent: %s", why);
        set_local(t, true);
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
    case RS_RADIO: snprintf(out, cap, "MHz, kHz, SF, CR"); break;
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
        set_local("Log in as admin first (Log in on the repeater's details).", true);
        return;
    }
    if (!rs_row_editable(i)) {
        return; // an INFO row: not selectable, so only a console key gets here
    }
    if (set_waiting()) {
        return; // the line already says "Sending the change..."
    }
    uint8_t st = mem->run.rs.st[i];
    if (rs_row_readable(i) && st != RS_V_VALUE) {
        set_local(st == RS_V_NONE ? "The repeater did not answer this one: Read again first." : "Still reading it.", true);
        return;
    }
    char heading[MESH_NAME_MAX + 64];
    if (row->kind == RS_PASSWORD) {
        snprintf(heading, sizeof(heading), "New admin password for\n%s", title);
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
    lichen_free(meshcore_self(), mem);
    mem       = NULL;
    reading   = false;
    menu_obj  = NULL;
    note      = again_row = NULL;
    memset(rows, 0, sizeof(rows));
}

static void add_rows(lv_obj_t *menu) {
    for (size_t i = 0; i < RS_ROWS; i++) {
        rs_row_t const *row = rs_row_at(i);
        rows[i]             = menu_add(menu, row->label, "", row->kind == RS_REBOOT ? restart_go : on_row,
                                       (void *)(uintptr_t)i);
        if (row->kind == RS_READONLY) {
            menu_set_kind(rows[i], MENU_INFO);
        } else if (row->kind == RS_PASSWORD) {
            menu_set_kind(rows[i], MENU_OPEN);
        } else if (row->kind == RS_REBOOT) {
            char ask[MESH_NAME_MAX + 80];
            snprintf(ask, sizeof(ask), "Restart %s? It is off the air for a while; log in again after.", title);
            menu_set_danger(rows[i], ask, "restart");
        } else if (row->kind == RS_RADIO) {
            menu_note(menu, "Careful: a wrong radio cuts the repeater off the mesh. Only a visit to the repeater undoes "
                            "it.");
        }
    }
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create_named(screen, "Repeater settings", title);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    meshcore_remote_hold(screen); // no scheduled status ask while the settings are read or set (#168)
    screen_obj = screen;
    menu_obj   = menu;
    reading    = false;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    lichen_free(meshcore_self(), mem);
    memset(rows, 0, sizeof(rows));
    note = again_row = NULL; // an older screen's, gone: never touched after a failed calloc below
    wipe_secrets();
    mem = lichen_alloc(meshcore_self(), sizeof(*mem), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!mem) {
        menu_note(menu, "Not enough memory.");
        return screen;
    }
    clear_local();
    rs_run_init(&mem->run, target);
    shown_busy = false;
    again_row  = menu_add(menu, "Read again", "", read_all, NULL);
    note       = menu_note(menu, "");
    add_rows(menu);
    shell_page_hint(screen, "Enter", "change", NULL);
    read_all(NULL); // the values are read as the page opens (gets only), as the Status screen asks as it opens
    timer = lv_timer_create(tick, TICK_MS, NULL);
    return screen;
}

void meshcore_remote_settings(uint8_t const prefix[6], char const *name) {
    memcpy(target, prefix, sizeof(target));
    snprintf(title, sizeof(title), "%s", name[0] ? name : "(no name)");
    shell_open_keyed(build, SCREEN_RSET, shell_key_hash(target, sizeof(target)));
}
