#pragma once

// The apps' lifecycle, the OS's side (#231; app_life.h has the states): main feeds the boot (background started, or
// switched off by safe mode), the shell feeds its screen stack, and stop(reason) (lichen_sys_apps_stop) generalises
// the apps' stop_for_update hooks. System surface: tools/layers.conf forbids lichen_sys_* in L6, apps never move
// their own state. The records sit in internal .bss under a spinlock that is a leaf: nothing else is taken under it
// and none of these but lichen_sys_apps_stop block.

#include "app_life.h"
#include "lichen_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// main.c's apps boot step, for the app of ctx (the index moves when apps leave, #520): its start_background said yes. Any task.
void lichen_sys_life_started(lichen_ctx_t ctx);
// main.c's apps boot step: safe mode switched the app's background part off (#232). Any task.
void lichen_sys_life_fault_crashes(lichen_ctx_t ctx);
// The shell's screen stack changed: ids[0..depth) bottom to top (nav_at) and ctxs[] the context each screen was opened
// in (lichen_ctx_self at the push). A screen belongs to the app in slot s when its id is SCREEN_APP_OWN + s or SCREEN_APP_BASE + s (its Settings
// page), else to the app whose context opened it (MeshCore's chat from its notification); the OS's pages belong to no
// app. At most LIFE_SCREENS_MAX screens are looked at. The LVGL task (the shell holds the LVGL lock); takes only the
// records' spinlock.
#define LIFE_SCREENS_MAX 8 // NAV_MAX_DEPTH
// The SD apps' loader (#523) is told when an app's last screen left the stack (OPEN or SUSPENDED to the rest state), with the
// app's ctx: on the LVGL task under the LVGL lock, after the records' spinlock is given, so cb only posts (no I/O, no
// waiting). The screen's widgets may still exist (the shell deletes them right after, or at the end of its animation).
// One callback, set once at boot; NULL takes it away. Any task (a pointer store).
typedef void (*lichen_life_close_cb_t)(lichen_ctx_t ctx);
void lichen_sys_life_on_close(lichen_life_close_cb_t cb);
void lichen_sys_life_screens(int const *ids, uint32_t const *ctxs, int depth);
// app_life_stop_begin / _stop_end / _wait_verdict for the app of ctx, with the tick clock (lichen_sys_life_now_ms,
// which a waiter's since_ms comes from). A ctx that left: BAD / LIFE_STATE_COUNT / FAILED. Any task.
life_begin_t lichen_sys_life_stop_begin(lichen_ctx_t ctx, life_reason_t reason);
life_state_t lichen_sys_life_stop_end(lichen_ctx_t ctx, bool stopped, uint32_t tasks_left);
life_wait_t  lichen_sys_life_wait(lichen_ctx_t ctx, life_reason_t own, uint32_t since_ms);
uint32_t     lichen_sys_life_now_ms(void);
// Whether the app of ctx may be unregistered now (app_life_removable of its record): false for a ctx that is no app's.
// Any task.
bool lichen_sys_life_removable(lichen_ctx_t ctx);
// A copy of the app's record; false for a ctx that left. Any task.
bool lichen_sys_life_get(lichen_ctx_t ctx, app_life_t *out);
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
