#pragma once

#include "hold_exit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Remote control of the screen (#190): the key lock ("fw ui lock") and what "fw ui open" accepts. Pure, host-tested
// in host_tests/remote_ctl_test.c; ui_remote.c holds the state under a spinlock (keyboard, console and LVGL tasks).
// Times are ms ticks (wrap-safe).

#define RLOCK_DEFAULT_S  60
#define RLOCK_MAX_S      600
#define RLOCK_ESC_HOLD_MS 3000 // the holder's way out: Esc held this long ends the lock
#define RLOCK_CODES      0x51 // TCA8418 key codes 1-0x50

typedef enum { RLOCK_END_NONE, RLOCK_END_TIMEOUT, RLOCK_END_ESC, RLOCK_END_CONSOLE } rlock_end_t;

// While on, physical keys are dropped. The lock ends by itself timeout_ms after the last console command (so a script
// that died can never leave the badge deaf), when Esc is held RLOCK_ESC_HOLD_MS, or with "fw ui lock off".
typedef struct {
    bool        on;
    uint32_t    timeout_ms;
    uint32_t    last_ms;             // the last console command
    hold_exit_t esc;
    bool        dropped[RLOCK_CODES]; // presses that were dropped: their releases are dropped too, also after the lock
    rlock_end_t ended;                // why the lock last ended, until rlock_take_end reads it
} rlock_t;

void rlock_on(rlock_t *l, uint32_t seconds, uint32_t now);
void rlock_off(rlock_t *l);
void rlock_touch(rlock_t *l, uint32_t now); // a console command ran: the timeout counts from now
// A physical key event. True = drop it (no wake, no raw hook, not LVGL). A press before the lock keeps its release.
bool rlock_key(rlock_t *l, uint8_t code, bool is_esc, bool pressed, uint32_t now);
// Ends the lock when its time is up or Esc has been held long enough; call regularly. True while the lock holds.
bool rlock_tick(rlock_t *l, uint32_t now);
uint32_t rlock_left_s(rlock_t const *l, uint32_t now); // seconds until the timeout (rounded up), 0 when off
rlock_end_t rlock_take_end(rlock_t *l);               // why it ended (once), RLOCK_END_NONE when nothing new
char const *rlock_end_name(rlock_end_t e);           // "timeout", "Esc held", "console"

// "fw ui lock [on [seconds] | off]": RLOCK_ARGS_SHOW for no arguments. *seconds: RLOCK_DEFAULT_S unless given (1 to
// RLOCK_MAX_S, digits only).
typedef enum { RLOCK_ARGS_ERR, RLOCK_ARGS_SHOW, RLOCK_ARGS_ON, RLOCK_ARGS_OFF } rlock_args_t;
rlock_args_t rlock_parse(char const *args, uint32_t *seconds);

// "fw ui open <app id | app name>": the index of the app whose id or name equals arg (case-insensitive, spaces around
// arg ignored; an id wins over another app's name), -1 for none
int rctl_find_app(char const *arg, char const *const *ids, char const *const *names, size_t n);
