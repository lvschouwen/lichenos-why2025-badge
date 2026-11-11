#pragma once
// The display fingerprint line (#354): one line with every suspect of the #254 flicker reports, so each report is
// comparable. Pure and ESP-free (host_tests/disp_state_test.c); disp_state.c gathers the values and prints it.

#include <stddef.h>
#include <stdint.h>

typedef enum { DS_TRIAL_NONE, DS_TRIAL_ARMED, DS_TRIAL_RUNNING, DS_TRIAL_PASSED } ds_trial_t;

// A missing value is NULL (strings), -1 (numbers and the yes/no flags) or an out-of-range trial: it prints "?".
// The *_raw strings are lichen_reg's text ("468 (3.0 s ago)", "? (not read yet)"): the part before the first space is
// the value, a leading '?' means none.
typedef struct {
    char const *reset;           // reset reason name of this boot ("sw", "panic", ...)
    char const *fw;              // "fw-<sha>"
    char const *psram_clk_raw;   // psram.clk, Hz
    int         psram_want_mhz;  // 80 or 120, -1 unknown
    int         trial;           // ds_trial_t, -1 unknown
    int         failed;          // 0/1, -1 unknown
    char const *pclk_raw;        // disp.pclk, Hz
    char const *refresh_raw;     // disp.refresh, dHz
    int         bl_req, kbd_req; // saved levels, -1 unknown
    char const *bl_raw, *kbd_raw; // bl.display, bl.keyboard (applied)
    char const *bl_boot_raw, *bl_idle_raw, *bl_override_raw, *bl_freq_raw;
    int         c6_up;           // 0/1, -1 unknown
    int64_t     c6_restarts;     // -1 unknown
    int64_t     uptime_s;        // -1 unknown
} ds_info_t;

// "disp state: boot sw fw-abc | psram 120 want 120 trial passed failed no | pclk 36000000 refresh 468 | bl 40/40 kbd
// 0/0 boot on idle bright override none freq 1000 | c6 up restarts 0 | uptime 65"; cut to cap, NUL-terminated.
// Returns the length written.
size_t ds_format(ds_info_t const *in, char *out, size_t cap);
