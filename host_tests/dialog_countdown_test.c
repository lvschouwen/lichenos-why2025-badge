// Host tests for the OS warning dialog's countdown (#14 T-B)
#include "dialog_countdown.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void runs_out_into_confirm(void) {
    dlg_countdown_t d;
    dlg_countdown_start(&d, 1000, 30);
    CHECK(dlg_countdown_left_s(&d, 1000) == 30);
    CHECK(dlg_countdown_left_s(&d, 1001) == 30);
    CHECK(dlg_countdown_left_s(&d, 2000) == 29);
    CHECK(dlg_countdown_left_s(&d, 30999) == 1);
    CHECK(dlg_countdown_tick(&d, 30999) == DLG_WAIT);
    // nobody pressed anything (the keyboard may be dead): the default is the confirm
    CHECK(dlg_countdown_tick(&d, 31000) == DLG_CONFIRM);
    CHECK(dlg_countdown_left_s(&d, 31000) == 0);
    // once
    CHECK(dlg_countdown_tick(&d, 32000) == DLG_WAIT);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 0) == DLG_WAIT);
}

static void keys_decide_once(void) {
    dlg_countdown_t d;
    dlg_countdown_start(&d, 0, 30);
    CHECK(dlg_countdown_key(&d, DLG_KEY_OTHER, 0) == DLG_WAIT);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 0) == DLG_CONFIRM);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ESC, 0) == DLG_WAIT);
    CHECK(dlg_countdown_tick(&d, 60000) == DLG_WAIT);

    dlg_countdown_start(&d, 0, 30);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ESC, 0) == DLG_CANCEL);
    // cancelled: running out later restarts nothing
    CHECK(dlg_countdown_tick(&d, 60000) == DLG_WAIT);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 0) == DLG_WAIT);
}

static void time_wraps(void) {
    dlg_countdown_t d;
    dlg_countdown_start(&d, 0xFFFFF000u, 10);
    CHECK(dlg_countdown_left_s(&d, 0x00000000u) == 6); // 4096 ms gone
    CHECK(dlg_countdown_tick(&d, 0x00000000u) == DLG_WAIT);
    CHECK(dlg_countdown_tick(&d, 0xFFFFF000u + 10000u) == DLG_CONFIRM);
}

// Format (#99): nobody answering is a no, and an Enter in the first second (held over from the row) is ignored
static void careful_runs_out_into_cancel(void) {
    dlg_countdown_t d;
    dlg_countdown_start_careful(&d, 1000, 15);
    dlg_countdown_enter_up(&d);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 1000) == DLG_WAIT);
    dlg_countdown_enter_up(&d);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 1000 + DLG_CAREFUL_ARM_MS - 1) == DLG_WAIT);
    CHECK(dlg_countdown_tick(&d, 15999) == DLG_WAIT);
    CHECK(dlg_countdown_tick(&d, 16000) == DLG_CANCEL);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 16000) == DLG_WAIT);

    dlg_countdown_start_careful(&d, 1000, 15);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ESC, 1000) == DLG_CANCEL); // Esc always counts
    dlg_countdown_start_careful(&d, 1000, 15);
    dlg_countdown_enter_up(&d);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 1000 + DLG_CAREFUL_ARM_MS) == DLG_CONFIRM);
    CHECK(dlg_countdown_tick(&d, 16000) == DLG_WAIT);

    // a plain countdown is not armed late: the C6 dialog takes Enter at once
    dlg_countdown_start(&d, 1000, 15);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 1000) == DLG_CONFIRM);
}

// A held Enter that repeats (or one press reaching two dialogs) never answers: only a release, then a press, does
static void careful_needs_a_fresh_press(void) {
    dlg_countdown_t d;
    dlg_countdown_start_careful(&d, 0, 15);
    for (uint32_t t = 0; t <= 5000; t += 100) {
        CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, t) == DLG_WAIT);
    }
    dlg_countdown_enter_up(&d);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 5100) == DLG_CONFIRM);

    // released at the start, but the first press came inside the arm time: it used the release up, its repeats fail
    dlg_countdown_start_careful(&d, 0, 15);
    dlg_countdown_enter_up(&d);
    for (uint32_t t = 500; t <= 5000; t += 100) {
        CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, t) == DLG_WAIT);
    }
    dlg_countdown_enter_up(&d);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER, 5100) == DLG_CONFIRM);
}

int main(void) {
    runs_out_into_confirm();
    keys_decide_once();
    time_wraps();
    careful_runs_out_into_cancel();
    careful_needs_a_fresh_press();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dialog_countdown_test: all passed\n");
    return 0;
}
