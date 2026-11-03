// Host test for the shared UI widgets' text and focus choices (#135): hints, status, empty state, countdown, focus keys
#include "ui_text.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(got, want)                                                                                           \
    do {                                                                                                               \
        if (strcmp((got), (want)) != 0) {                                                                              \
            printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (got), (want));                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_hint(void) {
    char b[64];
    ui_hint_build(b, sizeof(b), "Enter", "open", "Del", "delete", NULL);
    CHECK_STR(b, "Enter: open \xE2\x80\xA2 Del: delete");
    ui_hint_build(b, sizeof(b), "Enter", "open", NULL);
    CHECK_STR(b, "Enter: open");
    CHECK(ui_hint_build(b, sizeof(b), NULL) == 0 && b[0] == '\0');
    ui_hint_build(b, sizeof(b), "Enter", "open", "Del", NULL); // a pair without a verb ends the list
    CHECK_STR(b, "Enter: open");
    char small[16];
    size_t n = ui_hint_build(small, sizeof(small), "Enter", "open", "Del", "delete", NULL);
    CHECK(n == strlen(small) && n < sizeof(small));
    CHECK(ui_hint_build(small, 0, "a", "b", NULL) == 0);
    ui_confirm_hint(b, sizeof(b), "restart", NULL);
    CHECK_STR(b, "Enter: restart \xE2\x80\xA2 Esc: cancel");
    ui_confirm_hint(b, sizeof(b), "delete", "keep");
    CHECK_STR(b, "Enter: delete \xE2\x80\xA2 Esc: keep");
}

static void test_countdown(void) {
    char b[200];
    ui_countdown_text(b, sizeof(b), "Delete this file?", "(cancels in ", true, NULL, 10);
    CHECK_STR(b, "Delete this file? (cancels in 10 s)");
    ui_countdown_text(b, sizeof(b), "Restart the badge?", "(closes in ", true, NULL, 7); // 3 old wordings, one form
    CHECK_STR(b, "Restart the badge? (cancels in 7 s)");
    ui_countdown_text(b, sizeof(b), "Off?", "Cancels in ", true, NULL, 1);
    CHECK_STR(b, "Off? (cancels in 1 s)");
    ui_countdown_text(b, sizeof(b), "Set it?", "A wrong radio needs a visit. (cancels in ", true, NULL, 5);
    CHECK_STR(b, "Set it? A wrong radio needs a visit. (cancels in 5 s)");
    ui_countdown_text(b, sizeof(b), "C6 not responding.", "restarting in ", false, "restart", 30);
    CHECK_STR(b, "C6 not responding. (restart in 30 s)");
    ui_countdown_text(b, sizeof(b), "C6 not responding.", NULL, false, NULL, 30);
    CHECK_STR(b, "C6 not responding. (confirms in 30 s)");
    ui_countdown_text(b, sizeof(b), "", "(cancels in ", true, NULL, 3);
    CHECK_STR(b, "(cancels in 3 s)");
    char small[12];
    ui_countdown_text(small, sizeof(small), "A long question?", "(cancels in ", true, NULL, 3);
    CHECK(strlen(small) == sizeof(small) - 1);
}

static void test_status(void) {
    char b[64];
    ui_status_begin_text(b, sizeof(b), "Reading");
    CHECK_STR(b, "Reading...");
    ui_status_progress_text(b, sizeof(b), "Reading", 3, 17);
    CHECK_STR(b, "Reading 3 of 17");
    ui_status_progress_text(b, sizeof(b), "Reading", 20, 17);
    CHECK_STR(b, "Reading 17 of 17");
    ui_status_progress_text(b, sizeof(b), "Reading", -1, 17);
    CHECK_STR(b, "Reading 0 of 17");
    ui_status_progress_text(b, sizeof(b), "Clearing", 4, 0);
    CHECK_STR(b, "Clearing 4");
    ui_status_fail_text(b, sizeof(b), "The card is not there", "Insert it and try again");
    CHECK_STR(b, "The card is not there. Insert it and try again");
    ui_status_fail_text(b, sizeof(b), "No reply.", NULL);
    CHECK_STR(b, "No reply.");
    ui_status_fail_text(b, sizeof(b), "No reply", "");
    CHECK_STR(b, "No reply.");

    ui_status_t s = {0};
    CHECK(!ui_status_begin(&s, "Reading") && s.state == UI_STATUS_RUNNING);
    CHECK_STR(s.verb, "Reading");
    CHECK(ui_status_begin(&s, "Clearing")); // a begin that never ended is reported
    CHECK(ui_status_end(&s, false) && s.state == UI_STATUS_DONE);
    CHECK(!ui_status_end(&s, true) && s.state == UI_STATUS_DONE); // nothing running
    CHECK(!ui_status_begin(&s, "Again"));
    CHECK(ui_status_end(&s, true) && s.state == UI_STATUS_FAILED);
}

static void test_empty(void) {
    char b[96];
    ui_empty_text(b, sizeof(b), "contacts", "they appear when a node adverts");
    CHECK_STR(b, "No contacts yet: they appear when a node adverts.");
    ui_empty_text(b, sizeof(b), "notes", "Press N to write one.");
    CHECK_STR(b, "No notes yet: Press N to write one.");
    ui_empty_text(b, sizeof(b), "files", NULL);
    CHECK_STR(b, "No files yet.");
}

static void test_hint_pad(void) {
    CHECK(ui_hint_pad(0, 22, 18, 8) == 48);      // no padding yet: line + edge + gap
    CHECK(ui_hint_pad(36, 22, 18, 8) == 48);     // a screen with less than that is raised to it
    CHECK(ui_hint_pad(60, 22, 18, 8) == 60);     // more than that stays
    CHECK(ui_hint_pad(ui_hint_pad(0, 22, 18, 8), 22, 18, 8) == 48); // a rebuild does not add it twice
    CHECK(ui_hint_pad(0, 0, 18, 8) == 26);       // an unmeasured line must not be trusted as the hint's height
}

int main(void) {
    test_hint();
    test_countdown();
    test_status();
    test_empty();
    test_hint_pad();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
