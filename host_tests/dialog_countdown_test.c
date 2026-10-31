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
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER) == DLG_WAIT);
}

static void keys_decide_once(void) {
    dlg_countdown_t d;
    dlg_countdown_start(&d, 0, 30);
    CHECK(dlg_countdown_key(&d, DLG_KEY_OTHER) == DLG_WAIT);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER) == DLG_CONFIRM);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ESC) == DLG_WAIT);
    CHECK(dlg_countdown_tick(&d, 60000) == DLG_WAIT);

    dlg_countdown_start(&d, 0, 30);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ESC) == DLG_CANCEL);
    // cancelled: running out later restarts nothing
    CHECK(dlg_countdown_tick(&d, 60000) == DLG_WAIT);
    CHECK(dlg_countdown_key(&d, DLG_KEY_ENTER) == DLG_WAIT);
}

static void time_wraps(void) {
    dlg_countdown_t d;
    dlg_countdown_start(&d, 0xFFFFF000u, 10);
    CHECK(dlg_countdown_left_s(&d, 0x00000000u) == 6); // 4096 ms gone
    CHECK(dlg_countdown_tick(&d, 0x00000000u) == DLG_WAIT);
    CHECK(dlg_countdown_tick(&d, 0xFFFFF000u + 10000u) == DLG_CONFIRM);
}

int main(void) {
    runs_out_into_confirm();
    keys_decide_once();
    time_wraps();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dialog_countdown_test: all passed\n");
    return 0;
}
