#pragma once

#include <stdbool.h>

// The bench console scope (#290) as the shell sees it (#377): a remote key may answer a dialog or a radio page that
// makes the badge transmit only while it is set. MeshCore owns the scope (mesh_cscope.c, "fw mesh scope console
// <region> YES") and binds its own check here, so the shell reads the one notion of it without depending on the app.
// Pure, host-tested in host_tests/bench_scope_test.c.

typedef bool (*bench_scope_fn)(void); // true while the console scope is set; any task, never blocks long

// Any task; NULL unbinds. The last bind wins.
void bench_scope_bind(bench_scope_fn fn);
// Any task. False while nothing is bound (MeshCore not started): fail closed.
bool bench_scope_set(void);
