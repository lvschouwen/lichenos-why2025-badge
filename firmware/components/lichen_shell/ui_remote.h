#pragma once

#include <stdbool.h>

// Remote control for bench walks (#190): "fw ui lock" keeps the person holding the badge from typing into a scripted
// walk (physical keys only; "fw keys send" still works, its phys keys do not), "fw ui open" jumps to an app as from its home tile. The
// rules are remote_ctl.c; this is the glue (keyboard gate, console activity, the status bar banner).

void ui_remote_start(void);               // shell_start: the keyboard gate and the console activity hook
bool ui_remote_locked(void);              // any task
void ui_remote_report(void);              // "fw ui": prints the "ui lock: ..." line
void ui_remote_lock_cmd(char const *args); // "fw ui lock [on [s] | off]" (console task)
void ui_remote_open_cmd(char const *args); // "fw ui open <app id | app name | home>" (console task)
// The stress run (#278): the lock on (for RLOCK_MAX_S after the last command or ui_remote_touch) or off, as "fw ui lock
// on|off" without the reply. Not on the LVGL task (it takes the LVGL lock for the banner).
void ui_remote_hold(bool on);
void ui_remote_touch(void); // a long job counts as console activity: the lock's timeout starts again. Any task.
