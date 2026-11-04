// Host test: keyboard light pulse decision, pattern and flashlight levels (#164)
#include "attention_policy.h"
#include "backlight_level.h"

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
    attn_state_t ok = {.screen_lit = true};
    CHECK(attn_is_dm("dm:ab12") && !attn_is_dm("ch:badgetest") && !attn_is_dm("") && !attn_is_dm(NULL));
    CHECK(attn_wants_pulse(ATTN_DMS, "dm:ab12", 0, ok));
    CHECK(!attn_wants_pulse(ATTN_DMS, "ch:badgetest", 0, ok));
    CHECK(!attn_wants_pulse(ATTN_DMS, NULL, 0, ok));
    CHECK(attn_wants_pulse(ATTN_ALL, "ch:badgetest", 0, ok));
    CHECK(attn_wants_pulse(ATTN_ALL, NULL, 1, ok));
    CHECK(!attn_wants_pulse(ATTN_OFF, "dm:ab12", 0, ok));
    CHECK(!attn_wants_pulse(ATTN_ALL, "dm:ab12", 2, ok)); // an error does not pulse
    attn_state_t s = ok;
    s.dnd = true;
    CHECK(!attn_wants_pulse(ATTN_ALL, "dm:x", 0, s));
    s = ok;
    s.screen_lit = false;
    CHECK(!attn_wants_pulse(ATTN_ALL, "dm:x", 0, s));
    s = ok;
    s.hot = true;
    CHECK(!attn_wants_pulse(ATTN_ALL, "dm:x", 0, s));
    s = ok;
    s.flashlight = true;
    CHECK(!attn_wants_pulse(ATTN_ALL, "dm:x", 0, s));

    CHECK(attn_mode_from_u8(0) == ATTN_OFF && attn_mode_from_u8(2) == ATTN_ALL && attn_mode_from_u8(9) == ATTN_MODE_DEFAULT);
    CHECK(attn_mode_next(ATTN_OFF) == ATTN_DMS && attn_mode_next(ATTN_DMS) == ATTN_ALL && attn_mode_next(ATTN_ALL) == ATTN_OFF);

    CHECK(attn_keyboard_at(0) == ATTN_PEAK && attn_keyboard_at(149) == ATTN_PEAK);
    CHECK(attn_keyboard_at(150) == 0 && attn_keyboard_at(299) == 0);
    CHECK(attn_keyboard_at(300) == ATTN_PEAK && attn_keyboard_at(449) == ATTN_PEAK);
    CHECK(attn_keyboard_at(ATTN_PULSE_MS) == -1 && attn_keyboard_at(100000) == -1);

    attn_levels_t l = attn_flash_levels(false);
    CHECK(l.display == 100 && l.keyboard == 100);
    l = attn_flash_levels(true);
    CHECK(l.display == BACKLIGHT_DISPLAY_MIN && l.keyboard == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("attention_policy_test OK\n");
    return 0;
}
