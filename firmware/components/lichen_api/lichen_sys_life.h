#pragma once

// The apps' lifecycle, the OS's side (#231; app_life.h has the states): main feeds the boot (background started, or
// switched off by safe mode), the shell feeds its screen stack, and stop(reason) (lichen_sys_apps_stop) generalises
// the apps' stop_for_update hooks. System surface: tools/layers.conf forbids lichen_sys_* in L6, apps never move
// their own state. The records sit in internal .bss under a spinlock that is a leaf: nothing else is taken under it
// and none of these but lichen_sys_apps_stop block.

#include "app_life.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// main.c's apps boot step, for the app at index (lichen_app_at's): its start_background said yes. Any task.
void lichen_sys_life_started(size_t index);
// main.c's apps boot step: safe mode switched the app's background part off (#232). Any task.
void lichen_sys_life_fault_crashes(size_t index);
// The shell's screen stack changed: ids[0..depth) bottom to top (nav_at) and ctxs[] the context each screen was opened
// in (lichen_ctx_self at the push). A screen belongs to app i when its id is 1 + i or SCREEN_APP_BASE + i (its Settings
// page), else to the app whose context opened it (MeshCore's chat from its notification); the OS's pages belong to no
// app. At most LIFE_SCREENS_MAX screens are looked at. The LVGL task (the shell holds the LVGL lock); takes only the
// records' spinlock.
#define LIFE_SCREENS_MAX 8 // NAV_MAX_DEPTH
void lichen_sys_life_screens(int const *ids, uint32_t const *ctxs, int depth);
// app_life_stop_begin / _stop_end / _wait_verdict for the app at index, with the tick clock (lichen_sys_life_now_ms,
// which a waiter's since_ms comes from). An index past the table: BAD / LIFE_STATE_COUNT / FAILED. Any task.
life_begin_t lichen_sys_life_stop_begin(size_t index, life_reason_t reason);
life_state_t lichen_sys_life_stop_end(size_t index, bool stopped, uint32_t tasks_left);
life_wait_t  lichen_sys_life_wait(size_t index, life_reason_t own, uint32_t since_ms);
uint32_t     lichen_sys_life_now_ms(void);
// A copy of the app's record; false past the table. Any task.
bool lichen_sys_life_get(size_t index, app_life_t *out);
// "fw app life": every app's state and its last stop (console task)
void lichen_sys_life_console(void);

// How many booked timers (lichen_timer.h) ctx (a lichen_ctx_t) has. Any task (an atomic read).
size_t lichen_sys_timer_count(uint32_t ctx);
// Deletes every booked timer of ctx; stop(reason) queues it with lichen_sys_ui_call. LVGL task only.
void   lichen_sys_timer_drop(uint32_t ctx);
// ctx's app stopped: its booked timers are dead from now on, whether or not the drop got onto the LVGL task (a busy
// lock): each one deletes itself at its next tick, a start or a stop with a stale handle is refused. Any task (atomic).
void   lichen_sys_timer_kill(uint32_t ctx);
// ctx's app started again after a stop (lichen_sys_life_started): it may book timers again. Any task (atomic).
void   lichen_sys_timer_revive(uint32_t ctx);

// stop(reason) (#231): every registered app in registration order, each inside its own context. An app's
// stop_for_update hook (if it has one) runs on the calling task and is given until app_life_deadline_ms(reason): it is
// never interrupted, a late or refused one is reported (FAULTED) and its tasks are only counted, never deleted;
// its booked timers (lichen_timer.h) go with it. While another caller is stopping an app, this one waits for
// that verdict up to the deadline instead of running the hook twice at once. True when every hook said it stopped (an
// app without a hook has nothing running to stop). Holds no lock while a hook runs or while it waits. Blocks for
// seconds: never on the LVGL task, nor with the LVGL or the mesh lock held (a hook may wait for either).
bool lichen_sys_apps_stop(life_reason_t reason);

#ifdef __cplusplus
}
#endif
