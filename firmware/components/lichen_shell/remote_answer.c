// The remote key rules of #377: see remote_answer.h
#include "remote_answer.h"

#include "screen_ids.h"
#include "ui_keys.h"

#include <stddef.h>
#include <string.h>

// LVGL's key codes (ui_answer.c static_asserts them)
#define KEY_TAB   9u
#define KEY_ENTER 10u
#define KEY_RIGHT 19u
#define KEY_LEFT  20u

rkey_t remote_key_kind(uint8_t code, remote_code_for_fn code_for) {
    uint8_t shift;
    if (!code) {
        return RKEY_OTHER;
    }
    if (code == code_for(KEY_ENTER, &shift)) {
        return RKEY_ENTER;
    }
    if (code == code_for(KEY_TAB, &shift)) {
        return RKEY_TAB;
    }
    return code == code_for(KEY_LEFT, &shift) || code == code_for(KEY_RIGHT, &shift) ? RKEY_SIDE : RKEY_OTHER;
}

bool remote_holds_ok(uint32_t const *keys, size_t n, uint8_t (*code_of)(uint32_t key), remote_code_for_fn code_for,
                     size_t *at) {
    for (size_t i = 1; i < n; i++) { // ui_keys_parse puts a hold right after its key
        if (UI_KEY_IS_HOLD(keys[i]) && ui_keys_is_key(keys[i - 1]) &&
            remote_key_kind(code_of(keys[i - 1]), code_for) != RKEY_OTHER) {
            *at = i;
            return false;
        }
    }
    return true;
}

// The contact page's rows that open a page which sends as it is built, or asks for a password or command to send. A
// prefix match, so a row whose label grows ("Status history") is refused too rather than let through.
// Settings (a repeater's) opens SCREEN_RSET, which reads with CLI gets as it opens; Log out sends the logout.
static char const *const CONTACT_SEND_ROWS[] = {"Trace path", "Discover path", "Status", "Log in",
                                                "Command",    "Settings",      "Log out"};
// Rows on any screen whose Enter goes on air with no dialog: MeshCore's settings (Lichen Settings > Apps > MeshCore and
// MeshCore's own) send an advert at once (the grid's Advertise asks first), and start MeshCore (mesh_run.c's action
// label), which may send a due periodic advert
static char const *const ANY_SEND_ROWS[] = {"Send advert", "Start MeshCore"};

static bool row_is(char const *focus, char const *row) {
    while (*focus == '*' || *focus == ' ') {
        focus++; // a checked row's "* "
    }
    return strncmp(focus, row, strlen(row)) == 0;
}

bool remote_contact_send_row(char const *focus) {
    if (!focus || !*focus) {
        return true; // no row read: fail closed
    }
    for (size_t i = 0; i < sizeof(CONTACT_SEND_ROWS) / sizeof(CONTACT_SEND_ROWS[0]); i++) {
        if (row_is(focus, CONTACT_SEND_ROWS[i])) {
            return true;
        }
    }
    return false;
}

static bool any_send_row(char const *focus) {
    for (size_t i = 0; focus && i < sizeof(ANY_SEND_ROWS) / sizeof(ANY_SEND_ROWS[0]); i++) {
        if (row_is(focus, ANY_SEND_ROWS[i])) {
            return true;
        }
    }
    return false;
}

// Advert & contacts (SCREEN_ADVERT, #405): the rows a remote Enter may take there. Latitude and Longitude only open a
// text field (saving what is typed in it is hand-only: meshcore_advert_settings.c asks the key's cause as it saves).
// Exact labels, so a row that is renamed or grows ("Latitude source") is hand-only until it is listed here: every
// other row, known or new, changes what the badge sends on its own later (the interval, the flood mode, the location
// shared, who is added). The page refuses an injected key's change itself too: this rule reads the focus a key or two
// ahead of LVGL, so it is a convenience (a clear reply), not the lock.
static char const *const ADVERT_OPEN_ROWS[] = {"Latitude", "Longitude"};

static bool row_in(char const *focus, char const *const *rows, size_t n) {
    while (focus && (*focus == '*' || *focus == ' ')) {
        focus++; // a checked row's "* "
    }
    for (size_t i = 0; focus && i < n; i++) {
        if (strcmp(focus, rows[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool remote_advert_opens_field(char const *focus) {
    return row_in(focus, ADVERT_OPEN_ROWS, sizeof(ADVERT_OPEN_ROWS) / sizeof(ADVERT_OPEN_ROWS[0]));
}

static bool chat_screen(int id) {
    return id == SCREEN_CHAT || id == SCREEN_DM;
}

// Pages whose Enter (Again, Ask again, Log in, Command..., Read again, an edited value's set) goes on air with no
// dialog of its own
static bool send_page(int id) {
    return id == SCREEN_TRACE || id == SCREEN_STATUS || id == SCREEN_TELEMETRY || id == SCREEN_LOGIN ||
           id == SCREEN_CLI || id == SCREEN_RSET;
}

static rans_t chat_rule(remote_view_t const *v) {
    bool test = v->chat_known && !v->chat_dm && v->chat_name && strcmp(v->chat_name, REMOTE_TEST_CHANNEL) == 0;
    return test ? RANS_OK : RANS_CHAT_TEST;
}

// Enter with no dialog: the screen on top decides
static rans_t page_enter(remote_view_t const *v) {
    if (remote_radio_page(v->top_screen)) {
        return v->scope_set ? RANS_OK : RANS_NEEDS_SCOPE;
    }
    if (chat_screen(v->top_screen)) {
        return chat_rule(v);
    }
    if (v->top_screen == SCREEN_ADVERT && !v->enter_starts) { // a start is refused below, whatever the row
        return remote_advert_opens_field(v->focus) ? RANS_OK : RANS_ADVERT_HAND; // no row read: fail closed
    }
    if (send_page(v->top_screen) || v->enter_starts ||
        (v->top_screen == SCREEN_CONTACT && remote_contact_send_row(v->focus)) ||
        any_send_row(v->focus)) {
        return RANS_SENDS;
    }
    return RANS_OK;
}

bool remote_radio_page(int screen_id) {
    return screen_id == SCREEN_RADIO || screen_id == SCREEN_CUSTOM || screen_id == SCREEN_SWITCH;
}

static rans_t by_verdict(dlg_remote_verdict_t verdict, bool scope_set) {
    if (verdict == DLG_REMOTE_ALLOW) {
        return RANS_OK;
    }
    if (verdict == DLG_REMOTE_ALLOW_ONAIR_SCOPED) {
        return scope_set ? RANS_OK : RANS_NEEDS_SCOPE;
    }
    return RANS_HAND_ONLY; // REFUSE and anything else: fail closed
}

rans_t remote_key_check(rkey_t key, remote_view_t const *v) {
    if (key == RKEY_OTHER) {
        return RANS_OK;
    }
    if (key == RKEY_ENTER && v->dialog != RDLG_NONE) {
        // The Enter goes to the dialog (or to one about to be built), not to the page below it
        return v->dialog == RDLG_OPEN ? by_verdict(v->verdict, v->scope_set) : RANS_UNSURE;
    }
    if (key == RKEY_ENTER) {
        return page_enter(v);
    }
    // Tab, Left and Right reach the page even under a dialog's state being unsure: its rule holds either way
    if (key == RKEY_TAB) {
        return chat_screen(v->top_screen) ? chat_rule(v) : RANS_OK; // Tab-retry sends again
    }
    if (v->top_screen == SCREEN_ADVERT) {
        return RANS_ADVERT_HAND; // any row's: a cycle row's changes the interval, the flood or the location shared
    }
    return remote_radio_page(v->top_screen) && !v->scope_set ? RANS_NEEDS_SCOPE : RANS_OK;
}

// "#<digits>" that fits in 32 bits
static bool parse_gen(char const *p, size_t len, uint32_t *gen) {
    uint64_t v = 0;
    if (len < 2 || len > 11 || p[0] != '#') {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(p[i] - '0');
    }
    *gen = (uint32_t)v;
    return v <= UINT32_MAX;
}

bool remote_answer_parse_yes(char const *arg, char *label, size_t cap, bool *has_gen, uint32_t *gen) {
    *has_gen = false;
    while (*arg == ' ') {
        arg++;
    }
    size_t len = strlen(arg);
    while (len && arg[len - 1] == ' ') {
        len--;
    }
    size_t word = len; // where the last word starts
    while (word && arg[word - 1] != ' ') {
        word--;
    }
    if (word < len && arg[word] == '#') {
        if (!parse_gen(arg + word, len - word, gen)) {
            return false;
        }
        *has_gen = true;
        len      = word;
        while (len && arg[len - 1] == ' ') {
            len--;
        }
    }
    if (!len || len >= cap) {
        return false;
    }
    memcpy(label, arg, len);
    label[len] = '\0';
    return true;
}

rans_t remote_answer_check(remote_view_t const *v, char const *want_label, uint32_t const *want_gen) {
    if (v->dialog == RDLG_NONE) {
        return RANS_NO_DIALOG;
    }
    if (v->dialog != RDLG_OPEN) {
        return RANS_UNSURE;
    }
    if (!want_label || !*want_label || !v->label || strcmp(v->label, want_label) != 0) {
        return RANS_OTHER_DIALOG; // a stale or different dialog is never confirmed
    }
    if (want_gen && v->gen != *want_gen) {
        return RANS_OTHER_DIALOG; // the same words, but another open of them
    }
    return by_verdict(v->verdict, v->scope_set);
}

rans_t remote_answer_still(remote_view_t const *v, char const *want_label, uint32_t want_gen) {
    rans_t r = remote_answer_check(v, want_label, &want_gen);
    return r == RANS_NO_DIALOG ? RANS_OTHER_DIALOG : r; // it closed meanwhile: also not the one asked about
}

rans_t remote_dismiss_check(remote_view_t const *v) {
    return v->dialog == RDLG_OPEN ? RANS_OK : v->dialog == RDLG_NONE ? RANS_NO_DIALOG : RANS_UNSURE;
}

uint32_t remote_answer_wait_ms(uint32_t ms_open) {
    return ms_open >= REMOTE_ANSWER_ARM_MS ? 0 : REMOTE_ANSWER_ARM_MS - ms_open;
}
