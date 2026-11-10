#pragma once

#include <stdbool.h>
#include <stdint.h>

// The UI service's system surface (#225, #216 decision 1): the LVGL lock for the shell and main, never for an app
// (tools/layers.conf forbids lichen_sys_* in L6). The UI service in lichen_os is the only code that takes the port
// lock (board's port setup aside); everyone else comes through here, so the debug lock-owner check
// (CONFIG_LICHEN_UI_LOCK_CHECK_MODE) knows who holds it. Apps use lichen_ui_call (lichen_ui.h).

// Takes the (recursive) LVGL lock, as lvgl_port_lock: timeout_ms 0 waits forever. True when it is held; then call
// lichen_sys_ui_unlock once. Any task, the LVGL task included (it already holds the lock while LVGL runs its timers,
// so it never waits).
bool lichen_sys_ui_lock(uint32_t timeout_ms);
// Gives back one lichen_sys_ui_lock. Any task (the one that took it).
void lichen_sys_ui_unlock(void);
// Runs fn(arg) on the LVGL task, soon, as lichen_ui_call does, but waits for the lock up to timeout_ms (0 = forever,
// as lvgl_port_lock) instead of lichen_ui_call's 2 s. False when the lock was not free in time or there was no memory:
// fn will not run and arg is still the caller's. Any task, the LVGL task included.
bool lichen_sys_ui_call(void (*fn)(void *), void *arg, uint32_t timeout_ms);
// Takes back every fn(arg) queued with lichen_sys_ui_call or lichen_ui_call that has not started yet, as
// lv_async_call_cancel did: for a widget's delete handler whose memory arg is. Any task (takes the lock).
void lichen_sys_ui_cancel(void (*fn)(void *), void *arg);
// `fw ui lockcheck [reset]`: the lock-owner check's mode, whether it is armed, its hit count and the first call sites
// (CONFIG_LICHEN_UI_LOCK_CHECK_MODE), or resets the counts. Any task (the console's; it replies with console_reply).
void lichen_sys_ui_lockcheck_cmd(char const *args);
// The lock-owner check's hits since boot or the last reset (0 when it is not built in): the stress run (#278) fails a
// run during which it grew. Any task.
uint32_t lichen_sys_ui_lockcheck_hits(void);
