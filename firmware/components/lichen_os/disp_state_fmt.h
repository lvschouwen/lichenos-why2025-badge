#pragma once
// The display fingerprint line (#354): one line with every suspect of the #254 flicker reports, so each report is
// comparable. Pure and ESP-free (host_tests/disp_state_test.c); disp_state.c gathers the values and prints it.

#include <stddef.h>
#include <stdint.h>

// A missing value is NULL (strings), -1 (numbers and the yes/no flags): it prints "?".
// The *_raw strings are lichen_reg's text ("468 (3.0 s ago)", "? (not read yet)"): the part before the first space is
// the value, a leading '?' means none.
typedef struct {
    char const *reset;           // reset reason name of this boot ("sw", "panic", ...)
    char const *fw;              // "fw-<sha>"
    char const *psram_clk_raw;   // psram.clk, Hz
    int         psram_want_mhz;  // 80 or 120, -1 unknown
    int         failed;          // 0/1, -1 unknown
    int         arms;            // arm restarts in a row that never settled (#353), -1 unknown
    char const *fall_raw;        // psram.fail_why: the last fall ("none", "boot_check", ...)
    int64_t     heap_max_us;     // longest PSRAM heap check, -1 unknown or none yet
    char const *pclk_raw;        // disp.pclk, Hz
    char const *refresh_raw;     // disp.refresh, dHz
    int         bl_req, kbd_req; // saved levels, -1 unknown
    char const *bl_raw, *kbd_raw; // bl.display, bl.keyboard (applied)
    char const *bl_boot_raw, *bl_idle_raw, *bl_override_raw, *bl_freq_raw;
    int         c6_up;           // 0/1, -1 unknown
    int64_t     c6_restarts;     // -1 unknown
    int64_t     uptime_s;        // -1 unknown
} ds_info_t;

// "disp state: boot sw fw-abc | psram 120 want 120 failed no arms 0 fall none heapmax 7200us | pclk 36000000 refresh 468 | bl 40/40 kbd
// 0/0 boot on idle bright override none freq 1000 | c6 up restarts 0 | uptime 65"; cut to cap, NUL-terminated.
// Returns the length written.
size_t ds_format(ds_info_t const *in, char *out, size_t cap);
