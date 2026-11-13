// Host test: which Wi-Fi page row takes focus (#361)
#include "wifi_focus.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    bool const note_first[] = {false, true, true};
    CHECK(wifi_first_focusable(note_first, 3) == 1); // the "Saved networks" note is skipped
    bool const only_note[] = {false};
    CHECK(wifi_first_focusable(only_note, 1) == 1);  // nothing to focus: n
    CHECK(wifi_first_focusable(only_note, 0) == 0);
    bool const first[] = {true, false};
    CHECK(wifi_first_focusable(first, 2) == 0);
    if (failures) {
        return 1;
    }
    printf("wifi_focus_test: ok\n");
    return 0;
}
