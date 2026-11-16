// The status bar's chip against the clock (#399). Widths are those measured on the badge's small font.
#include "sb_layout.h"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define SCREEN 720
#define PAD    24
#define CLOCK  62 // "~00:00"
#define CLEAR  8
#define RIGHT  240 // Wi-Fi fan, "Wi-Fi", LoRa and chevrons with their gaps, no chip

// dot 10 + pad 8 + gap 6; the count adds its text and the 4 px column gap
#define DOT_W      24
#define COUNT_W(t) (DOT_W + 4 + (t))

int main(void) {
    int32_t room = sb_notify_room(SCREEN, PAD, CLOCK, CLEAR, RIGHT);
    CHECK(room == (SCREEN - PAD - RIGHT) - (SCREEN / 2 + CLOCK / 2) - CLEAR);

    // The old layout always drew dot and count: "99+" (36 px) overshoots the room, the clock is overlapped
    CHECK(COUNT_W(36) > room);
    // The new one drops the count and keeps the dot clear of the clock
    CHECK(sb_notify_fit(room, DOT_W, COUNT_W(36)) == SB_NOTIFY_DOT);
    CHECK(DOT_W <= room);

    CHECK(sb_notify_fit(room, DOT_W, COUNT_W(12)) == SB_NOTIFY_COUNT); // "1"
    CHECK(sb_notify_fit(room, DOT_W, COUNT_W(24)) == SB_NOTIFY_COUNT); // "99"
    CHECK(sb_notify_fit(DOT_W - 1, DOT_W, COUNT_W(12)) == SB_NOTIFY_NONE);
    CHECK(sb_notify_fit(-5, DOT_W, COUNT_W(12)) == SB_NOTIFY_NONE);
    CHECK(sb_notify_fit(COUNT_W(36), DOT_W, COUNT_W(36)) == SB_NOTIFY_COUNT); // exact fit
    printf(failures ? "sb_layout_test: %d failed\n" : "sb_layout_test ok\n", failures);
    return failures != 0;
}
