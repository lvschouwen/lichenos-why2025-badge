#pragma once

#include <stdbool.h>
#include <stddef.h>

// What the shell's remote key checks (#377) need to know of MeshCore, without depending on the app: MeshCore binds a
// function for each, and the shell reads them through here. Pure, host-tested in host_tests/bench_scope_test.c. Only
// MeshCore (mesh) may bind (tools/layers.conf, owner-only): another app binding would switch the policy off.
//   - The bench console scope (#290): a remote key may answer a dialog or a radio page that makes the badge transmit
//     only while it is set (mesh_cscope.c, "fw mesh scope console <region> YES").
//   - The chat on top of the stack: a remote Enter sends in a chat only when it is #badgetest (meshcore_chat.c).
//   - Whether Enter on the screen on top would start MeshCore (its grid while it is stopped, meshcore_grid.c): a start
//     may send a periodic advert that is due, so a remote Enter there is refused.

typedef bool (*bench_scope_fn)(void); // true while the console scope is set; any task, never blocks long
// The chat screen on top: false when the top screen is no chat; else its channel name ("#" included; a DM's "@name")
// in name (always terminated) and whether it is a DM. LVGL task.
typedef bool (*bench_chat_fn)(char *name, size_t cap, bool *dm);
typedef bool (*bench_starts_fn)(void); // LVGL task

// Any task; NULL unbinds. The last bind wins.
void bench_scope_bind(bench_scope_fn fn);
// Any task. False while nothing is bound (MeshCore not started): fail closed.
bool bench_scope_set(void);
// Any task; NULL unbinds. The last bind wins.
void bench_chat_bind(bench_chat_fn fn);
// LVGL task. False while nothing is bound or no chat is on top (name "", dm false then).
bool bench_chat_on_top(char *name, size_t cap, bool *dm);
// Any task; NULL unbinds. The last bind wins.
void bench_starts_bind(bench_starts_fn fn);
// LVGL task. False while nothing is bound (MeshCore's grid was never built, so it is not on top).
bool bench_enter_starts(void);
