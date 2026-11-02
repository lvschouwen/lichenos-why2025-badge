#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Screen timeout (#43): dims the backlight after a while without a key, then switches it off. Pure, host-tested in
// host_tests/idle_policy_test.c; screen_idle.c feeds it the idle clock and applies the levels.

typedef enum { IDLE_BRIGHT, IDLE_DIM, IDLE_OFF } idle_level_t;

// Seconds without a key before dimming / switching off; 0 = never
typedef struct {
    uint32_t dim_s;
    uint32_t off_s;
} idle_times_t;

// Settings > Display keeps both as a u8 in tens of seconds (NVS keys idle_dim / idle_off); 0 = off
#define IDLE_DIM_DEFAULT_10S 6  // 1 min
#define IDLE_OFF_DEFAULT_10S 30 // 5 min
// The screensaver and pocket lock switches (#75), u8 0/1 in the same store; the saver is on by default, the lock off
#define IDLE_SAVER_KEY "saver"
#define IDLE_LOCK_KEY  "lock"
// The saver's style (#119), u8: saver_style_t in saver_station.h, the station clock by default
#define IDLE_SAVER_STYLE_KEY "saver_style"

// What keeps the screen lit regardless of the idle clock. busy (an update running, the Bench app measuring): bright, and
// the clock starts again, so the timeout counts in full from when it ends. setup (the first-run screens): bright for at
// most IDLE_SETUP_HOLD_S since the last key, so an abandoned welcome does not drain the battery.
#define IDLE_SETUP_HOLD_S 600
typedef enum { IDLE_HOLD_NONE, IDLE_HOLD_BRIGHT, IDLE_HOLD_RESTART } idle_hold_t;
idle_hold_t idle_hold(bool busy, bool setup, uint32_t idle_s);

// What the backlight should be after idle_s seconds without a key. inhibit (any hold) keeps it bright. Off wins over dim when both are due (also when off_s <= dim_s).
idle_level_t idle_level(uint32_t idle_s, idle_times_t t, bool inhibit);

// What the screen shows at a level (#75): the clock screensaver takes the dim stage (at the dim backlight) when it is on;
// the pocket lock engages when the screen goes dark-ish (the saver or off) and stays until its key combo. forced: "fw
// screensaver test", the saver now (dim at least) even with dimming off; pass it only when nothing holds the screen.
typedef struct {
    idle_level_t level;
    bool         saver; // the clock screensaver covers the screens
    bool         lock;  // the lock engages (it is released by the combo, not by this)
} idle_stage_t;
idle_stage_t idle_stage(idle_level_t level, bool saver_on, bool lock_on, bool forced);

// A key was pressed while the screen showed `shown`: true when that key only wakes the screen and must not reach the
// UI: the screen was off, so the person could not see what it would do, or the screensaver hid the screen (#75). A
// plain dimmed screen is still readable: its key is delivered.
bool idle_key_swallowed(idle_stage_t shown);

idle_times_t idle_times_from_10s(uint8_t dim_10s, uint8_t off_10s);

// The choices a Settings row cycles through (Enter): the next one after current, wrapping; a value not in the list
// (an older firmware's) goes to the first choice above it, or back to the first.
uint8_t idle_dim_next(uint8_t current_10s);
uint8_t idle_off_next(uint8_t current_10s);
// "off", "30 s", "1 min", "30 min"
void idle_choice_text(uint8_t value_10s, char *out, size_t cap);
char const *idle_level_name(idle_level_t level);
