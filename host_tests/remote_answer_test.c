// Host test: what a key sent over the console may answer (#377): the dialog verdicts, the bench scope rule, the radio
// pages that are no dialogs, the label guard of "fw ui answer yes" and its open-number (gen) check
#include "kbd_map.h"
#include "remote_answer.h"
#include "screen_ids.h"
#include "ui_keys.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static remote_view_t dialog(dlg_remote_verdict_t verdict, char const *label, bool scope) {
    return (remote_view_t){.dialog     = RDLG_OPEN,
                           .verdict    = verdict,
                           .label      = label,
                           .gen        = 7,
                           .top_screen = SCREEN_SETTINGS,
                           .scope_set  = scope};
}

static remote_view_t page(int screen, bool scope) {
    return (remote_view_t){.dialog = RDLG_NONE, .verdict = DLG_REMOTE_REFUSE, .label = "", .top_screen = screen,
                           .scope_set = scope};
}

static void test_dialog_enter(void) {
    remote_view_t v = dialog(DLG_REMOTE_REFUSE, "reset settings", true);
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_HAND_ONLY); // the scope never opens a DANGER dialog
    CHECK(remote_key_check(RKEY_OTHER, &v) == RANS_OK);        // Esc is never blocked
    CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_OK);
    v = dialog(DLG_REMOTE_ALLOW, "restart", false);
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
    v = dialog(DLG_REMOTE_ALLOW_ONAIR_SCOPED, "send", false);
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_NEEDS_SCOPE);
    v.scope_set = true;
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
    v.verdict = (dlg_remote_verdict_t)42; // not a verdict: fail closed
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_HAND_ONLY);
    v.dialog = RDLG_SETTLING;
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_UNSURE);
    v.dialog = RDLG_UNREAD;
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_UNSURE);
    CHECK(remote_key_check(RKEY_OTHER, &v) == RANS_OK);
}

static void test_radio_pages(void) {
    int const scoped[] = {SCREEN_RADIO, SCREEN_CUSTOM, SCREEN_SWITCH};
    for (size_t i = 0; i < sizeof(scoped) / sizeof(scoped[0]); i++) {
        remote_view_t v = page(scoped[i], false);
        CHECK(remote_radio_page(scoped[i]));
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_NEEDS_SCOPE); // the Switch radio confirm, a preset
        CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_NEEDS_SCOPE);  // the TX power slider
        CHECK(remote_key_check(RKEY_OTHER, &v) == RANS_OK);          // Up, Down, Esc
        v.scope_set = true;
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
        CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_OK);
        v = page(scoped[i], false);
        v.dialog = RDLG_UNREAD; // a Left or Right goes to the page whatever the dialog state
        CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_NEEDS_SCOPE);
    }
    remote_view_t v = page(SCREEN_SETTINGS, false);
    CHECK(!remote_radio_page(SCREEN_SETTINGS) && !remote_radio_page(SCREEN_CHANNEL) && !remote_radio_page(-1));
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
    CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_OK);
    // A dialog over the radio page takes the Enter: its verdict counts, not the page's
    v = page(SCREEN_RADIO, false);
    v.dialog  = RDLG_OPEN;
    v.verdict = DLG_REMOTE_ALLOW;
    v.label   = "close";
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
}

static void test_answer_label(void) {
    remote_view_t v = dialog(DLG_REMOTE_ALLOW_ONAIR_SCOPED, "send", true);
    CHECK(remote_answer_check(&v, "send", NULL) == RANS_OK);
    CHECK(remote_answer_check(&v, "Send", NULL) == RANS_OTHER_DIALOG); // exact, a stale or different dialog never
    CHECK(remote_answer_check(&v, "sen", NULL) == RANS_OTHER_DIALOG);
    CHECK(remote_answer_check(&v, "", NULL) == RANS_OTHER_DIALOG);
    CHECK(remote_answer_check(&v, NULL, NULL) == RANS_OTHER_DIALOG);
    v.scope_set = false;
    CHECK(remote_answer_check(&v, "send", NULL) == RANS_NEEDS_SCOPE);
    v = dialog(DLG_REMOTE_REFUSE, "erase everything", true);
    CHECK(remote_answer_check(&v, "erase everything", NULL) == RANS_HAND_ONLY);
    v = dialog(DLG_REMOTE_ALLOW, "reset", false);
    CHECK(remote_answer_check(&v, "reset", NULL) == RANS_OK);
    v.label = NULL;
    CHECK(remote_answer_check(&v, "reset", NULL) == RANS_OTHER_DIALOG);
    v.dialog = RDLG_NONE;
    CHECK(remote_answer_check(&v, "reset", NULL) == RANS_NO_DIALOG);
    v.dialog = RDLG_SETTLING;
    CHECK(remote_answer_check(&v, "reset", NULL) == RANS_UNSURE);
}

static void test_answer_gen(void) {
    remote_view_t v = dialog(DLG_REMOTE_ALLOW, "delete", false);
    CHECK(remote_answer_still(&v, "delete", 7) == RANS_OK);
    CHECK(remote_answer_still(&v, "delete", 6) == RANS_OTHER_DIALOG); // the same words, opened again since
    CHECK(remote_answer_still(&v, "clear all", 7) == RANS_OTHER_DIALOG);
    v.dialog = RDLG_NONE; // answered by hand meanwhile
    CHECK(remote_answer_still(&v, "delete", 7) == RANS_OTHER_DIALOG);
    v = dialog(DLG_REMOTE_ALLOW_ONAIR_SCOPED, "send", true);
    CHECK(remote_answer_still(&v, "send", 7) == RANS_OK);
    v.scope_set = false; // the scope went in the wait
    CHECK(remote_answer_still(&v, "send", 7) == RANS_NEEDS_SCOPE);
}

// The badge's own code-for: kbd_map, the real TCA8418 layout, as lichen_sys_keys_code_for wraps it
static uint8_t real_code_for(uint32_t key, uint8_t *shift_code) {
    bool    shift = false;
    uint8_t code  = kbd_map_code_for(key, &shift);
    *shift_code   = 0; // the kind only reads the code
    (void)shift;
    return code;
}

static uint8_t seq_code(uint32_t key) {
    uint8_t shift;
    return UI_KEY_IS_SHAPE(key) ? UI_KEY_SHAPE_CODE(key) : real_code_for(key, &shift);
}

static void test_kind(void) {
    uint8_t shift;
    CHECK(remote_key_kind(real_code_for(10, &shift), real_code_for) == RKEY_ENTER);
    CHECK(remote_key_kind(real_code_for('\n', &shift), real_code_for) == RKEY_ENTER); // a "\n" in text: is Enter
    CHECK(remote_key_kind(real_code_for('\t', &shift), real_code_for) == RKEY_TAB);
    CHECK(remote_key_kind(real_code_for(20, &shift), real_code_for) == RKEY_SIDE);
    CHECK(remote_key_kind(real_code_for(19, &shift), real_code_for) == RKEY_SIDE);
    CHECK(remote_key_kind(real_code_for(27, &shift), real_code_for) == RKEY_OTHER); // Esc
    CHECK(remote_key_kind(real_code_for(17, &shift), real_code_for) == RKEY_OTHER);
    CHECK(remote_key_kind(real_code_for('a', &shift), real_code_for) == RKEY_OTHER);
    CHECK(remote_key_kind(real_code_for('A', &shift), real_code_for) == RKEY_OTHER);
    CHECK(remote_key_kind(2, real_code_for) == RKEY_OTHER); // a shape key
    CHECK(remote_key_kind(0, real_code_for) == RKEY_OTHER);
    CHECK(real_code_for(10, &shift) != 0 && real_code_for('\t', &shift) != 0);
}

static bool holds_ok(char const *text, size_t *at) {
    uint32_t keys[64];
    size_t   pos;
    size_t   n = ui_keys_parse(text, keys, 64, &pos);
    CHECK(n != UI_KEYS_ERR);
    return n != UI_KEYS_ERR && remote_holds_ok(keys, n, seq_code, real_code_for, at);
}

static void test_holds(void) {
    size_t at = 99;
    CHECK(holds_ok("esc hold:1500 bksp hold:1300", &at)); // the combo stays possible
    CHECK(holds_ok("down hold:500 up", &at));
    CHECK(holds_ok("square hold:3000", &at));
    CHECK(holds_ok("enter right left tab", &at)); // taps are fine
    CHECK(!holds_ok("enter hold:3000", &at) && at == 1);
    CHECK(!holds_ok("down enter phys hold:3000", &at) && at == 2); // the hold goes right after its key, over phys
    CHECK(!holds_ok("right hold:200", &at));
    CHECK(!holds_ok("left hold:200", &at));
    CHECK(!holds_ok("tab hold:200", &at));
}

static void test_parse_yes(void) {
    char     label[48];
    bool     has;
    uint32_t gen = 0;
    CHECK(remote_answer_parse_yes(" clear all ", label, sizeof(label), &has, &gen) && !has &&
          strcmp(label, "clear all") == 0);
    CHECK(remote_answer_parse_yes(" send #12", label, sizeof(label), &has, &gen) && has && gen == 12 &&
          strcmp(label, "send") == 0);
    CHECK(remote_answer_parse_yes("set region  #4294967295 ", label, sizeof(label), &has, &gen) && has &&
          gen == 4294967295u && strcmp(label, "set region") == 0);
    CHECK(!remote_answer_parse_yes("send #4294967296", label, sizeof(label), &has, &gen)); // over 32 bits
    CHECK(!remote_answer_parse_yes("send #", label, sizeof(label), &has, &gen));
    CHECK(!remote_answer_parse_yes("send #1x", label, sizeof(label), &has, &gen));
    CHECK(!remote_answer_parse_yes("#3", label, sizeof(label), &has, &gen)); // no label
    CHECK(!remote_answer_parse_yes("   ", label, sizeof(label), &has, &gen));
    CHECK(!remote_answer_parse_yes("a-label-that-is-far-too-long-for-the-dialog-layer-to-keep", label, 48, &has, &gen));
    // The gen pins the open: the same words opened again are another dialog
    remote_view_t v    = dialog(DLG_REMOTE_ALLOW, "delete", false);
    uint32_t      want = 7, other = 8;
    CHECK(remote_answer_check(&v, "delete", &want) == RANS_OK);
    CHECK(remote_answer_check(&v, "delete", &other) == RANS_OTHER_DIALOG);
}

static remote_view_t chat(int screen, char const *name, bool dm) {
    remote_view_t v = page(screen, true); // the bench scope does not open other chats
    v.chat_known    = true;
    v.chat_name     = name;
    v.chat_dm       = dm;
    return v;
}

static void test_chat(void) {
    remote_view_t v = chat(SCREEN_CHAT, "#badgetest", false);
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
    CHECK(remote_key_check(RKEY_TAB, &v) == RANS_OK);
    v = chat(SCREEN_CHAT, "#BadgeTest", false); // exact: another name is another channel
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_CHAT_TEST);
    v = chat(SCREEN_CHAT, "Public", false);
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_CHAT_TEST);
    CHECK(remote_key_check(RKEY_TAB, &v) == RANS_CHAT_TEST); // Tab-retry sends again
    CHECK(remote_key_check(RKEY_OTHER, &v) == RANS_OK);      // typing is fine
    CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_OK);
    v = chat(SCREEN_CHAT, "#badgetest2", false);
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_CHAT_TEST);
    v = chat(SCREEN_DM, "@badgetest", true);
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_CHAT_TEST);
    v = chat(SCREEN_DM, "#badgetest", true); // a DM never, whatever its name
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_CHAT_TEST);
    v            = chat(SCREEN_CHAT, "#badgetest", false);
    v.chat_known = false; // the chat could not be read: fail closed
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_CHAT_TEST);
    v.chat_known = true;
    v.chat_name  = NULL;
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_CHAT_TEST);
    v = page(SCREEN_SETTINGS, false); // Tab elsewhere only moves the focus
    CHECK(remote_key_check(RKEY_TAB, &v) == RANS_OK);
    // A dialog over the chat takes the Enter: its own verdict
    v         = chat(SCREEN_CHAT, "Public", false);
    v.dialog  = RDLG_OPEN;
    v.verdict = DLG_REMOTE_ALLOW;
    v.label   = "close";
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
}

static void test_send_pages(void) {
    int const pages[] = {SCREEN_TRACE, SCREEN_STATUS, SCREEN_TELEMETRY, SCREEN_LOGIN, SCREEN_CLI, SCREEN_RSET};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        remote_view_t v = page(pages[i], true);
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_SENDS); // the scope does not open them either
        CHECK(remote_key_check(RKEY_OTHER, &v) == RANS_OK); // Esc, Up, Down still free
        CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_OK);
    }
    char const *sends[] = {"Trace path", "Discover path", "Status", "Status history", "Log in",
                           "Command...", "Settings",      "Log out", "",              NULL};
    for (size_t i = 0; i < sizeof(sends) / sizeof(sends[0]); i++) {
        remote_view_t v = page(SCREEN_CONTACT, true);
        v.focus         = sends[i];
        CHECK(remote_contact_send_row(sends[i]));
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_SENDS);
    }
    // Posts opens the room's DM chat (the chat rule decides there); Request telemetry, Remove and Reset path ask first
    char const *fine[] = {"Message", "Posts", "Signal", "Favourite", "Share", "Request telemetry", "Remove", "Reset path"};
    for (size_t i = 0; i < sizeof(fine) / sizeof(fine[0]); i++) {
        remote_view_t v = page(SCREEN_CONTACT, false);
        v.focus         = fine[i];
        CHECK(!remote_contact_send_row(fine[i]));
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK); // their dialogs (or none: nothing sent) decide
    }
    remote_view_t v = page(SCREEN_SETTINGS, false);
    v.focus         = "Status"; // the row rule is the contact page's only
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
    // MeshCore's settings: Send advert goes at once, with no dialog; on any screen that has the row
    int const advert_rows[] = {SCREEN_SETTINGS, SCREEN_APP_BASE + 3};
    for (size_t i = 0; i < 2; i++) {
        v       = page(advert_rows[i], true);
        v.focus = "Send advert";
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_SENDS);
        v.focus = "* Send advert";
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_SENDS);
        v.focus = "Start MeshCore"; // starts it: a due periodic advert may go out
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_SENDS);
        v.focus = "Start at boot"; // only the setting
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
        v.focus = "Advert & contacts"; // opens the page; its own rule below
        CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
    }
    // An Enter that starts a stopped MeshCore (a due periodic advert goes out)
    v              = page(1, true); // an app's first screen (1 + its index: MeshCore's grid)
    v.enter_starts = true;
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_SENDS);
    CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_OK);
    v.enter_starts = false;
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK);
}

// #405: Advert & contacts, every row of meshcore_advert_settings.c's build() (its labels pinned here: a renamed row
// fails this test) x every key kind x the bench scope. Typing goes as RKEY_OTHER; Esc is RKEY_OTHER too.
typedef struct {
    char const *row;
    rans_t      enter_off, enter_on; // Enter without and with the bench scope
} advert_case_t;

static void test_advert_page(void) {
    advert_case_t const rows[] = {
        {"Advert every", RANS_ADVERT_HAND, RANS_ADVERT_HAND}, // the interval
        {"Next advert", RANS_ADVERT_HAND, RANS_ADVERT_HAND},  // an info row: not listed, so fail closed
        {"Advert button sends", RANS_ADVERT_HAND, RANS_ADVERT_HAND}, // the flood mode
        {"Latitude", RANS_OK, RANS_OK},                       // opens the text field
        {"Longitude", RANS_OK, RANS_OK},
        {"Share location in advert", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"* Share location in advert", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Auto-add", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Chat", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Repeaters", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Rooms", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Sensors", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Max hops", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Overwrite oldest when full", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Send advert", RANS_NEEDS_SCOPE, RANS_OK}, // a one-off send, as on the radio pages
        // Unknown, grown, partial or missing labels fail closed
        {"Latitude source", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Lat", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"latitude", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Send advert now", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"Some new row", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {"", RANS_ADVERT_HAND, RANS_ADVERT_HAND},
        {NULL, RANS_ADVERT_HAND, RANS_ADVERT_HAND},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        for (int scope = 0; scope < 2; scope++) {
            remote_view_t v = page(SCREEN_ADVERT, scope);
            v.focus         = rows[i].row;
            rans_t want     = scope ? rows[i].enter_on : rows[i].enter_off;
            if (remote_key_check(RKEY_ENTER, &v) != want) {
                printf("  advert row '%s' scope %d: Enter\n", rows[i].row ? rows[i].row : "(null)", scope);
            }
            CHECK(remote_key_check(RKEY_ENTER, &v) == want);
            CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_ADVERT_HAND); // Left, Right: hand-only on every row
            CHECK(remote_key_check(RKEY_TAB, &v) == RANS_OK);
            CHECK(remote_key_check(RKEY_OTHER, &v) == RANS_OK);         // Esc, Up, Down, typing
            v.dialog  = RDLG_OPEN; // a dialog over the page takes the Enter: its verdict, not the row
            v.verdict = DLG_REMOTE_REFUSE;
            v.label   = "x";
            CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_HAND_ONLY);
            CHECK(remote_key_check(RKEY_SIDE, &v) == RANS_ADVERT_HAND);
        }
    }
    CHECK(remote_advert_row("Latitude") == RADV_OPENS_FIELD && remote_advert_row("Longitude") == RADV_OPENS_FIELD);
    CHECK(remote_advert_row("Send advert") == RADV_SENDS_ONCE && remote_advert_row(NULL) == RADV_CHANGES_SENDING);
    // The rule is this page's only: elsewhere the rows are as before
    remote_view_t v = page(SCREEN_SETTINGS, true);
    v.focus         = "Send advert"; // MeshCore's settings: still refused outright
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_SENDS);
    v.focus = "Latitude";
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_OK && remote_key_check(RKEY_SIDE, &v) == RANS_OK);
    v = page(SCREEN_ADVERT, true);
    v.enter_starts = true; // no start path on this page, but a start is refused whatever the row
    v.focus        = "Latitude";
    CHECK(remote_key_check(RKEY_ENTER, &v) == RANS_SENDS);
}

static void test_wait(void) {
    CHECK(remote_answer_wait_ms(0) == REMOTE_ANSWER_ARM_MS);
    CHECK(remote_answer_wait_ms(400) == 700);
    CHECK(remote_answer_wait_ms(REMOTE_ANSWER_ARM_MS) == 0);
    CHECK(remote_answer_wait_ms(60000) == 0);
    CHECK(REMOTE_ANSWER_ARM_MS > 1000); // over the careful arming time (DLG_CAREFUL_ARM_MS)
}

int main(void) {
    test_dialog_enter();
    test_radio_pages();
    test_answer_label();
    test_answer_gen();
    test_wait();
    test_kind();
    test_holds();
    test_parse_yes();
    test_chat();
    test_send_pages();
    test_advert_page();
    if (failures) {
        printf("remote_answer_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("remote_answer_test: ok\n");
    return 0;
}
