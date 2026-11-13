// Host test: what a key sent over the console may answer (#377): the dialog verdicts, the bench scope rule, the radio
// pages that are no dialogs, the label guard of "fw ui answer yes" and its open-number (gen) check
#include "remote_answer.h"
#include "screen_ids.h"

#include <stdio.h>

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
    CHECK(remote_answer_check(&v, "send") == RANS_OK);
    CHECK(remote_answer_check(&v, "Send") == RANS_OTHER_DIALOG); // exact, a stale or different dialog never
    CHECK(remote_answer_check(&v, "sen") == RANS_OTHER_DIALOG);
    CHECK(remote_answer_check(&v, "") == RANS_OTHER_DIALOG);
    CHECK(remote_answer_check(&v, NULL) == RANS_OTHER_DIALOG);
    v.scope_set = false;
    CHECK(remote_answer_check(&v, "send") == RANS_NEEDS_SCOPE);
    v = dialog(DLG_REMOTE_REFUSE, "erase everything", true);
    CHECK(remote_answer_check(&v, "erase everything") == RANS_HAND_ONLY);
    v = dialog(DLG_REMOTE_ALLOW, "reset", false);
    CHECK(remote_answer_check(&v, "reset") == RANS_OK);
    v.label = NULL;
    CHECK(remote_answer_check(&v, "reset") == RANS_OTHER_DIALOG);
    v.dialog = RDLG_NONE;
    CHECK(remote_answer_check(&v, "reset") == RANS_NO_DIALOG);
    v.dialog = RDLG_SETTLING;
    CHECK(remote_answer_check(&v, "reset") == RANS_UNSURE);
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
    if (failures) {
        printf("remote_answer_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("remote_answer_test: ok\n");
    return 0;
}
