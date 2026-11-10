#pragma once
// The console scope on the badge (#290): what the senders and the console read of it. BadgeMesh holds the live name
// and key (mesh_link.h) and floods with them; this keeps a copy for the senders' refusal check, which must not wait on
// the mesh lock, and runs "fw mesh scope". The rules are mesh_scope.h's (host-tested).
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// After every load or set of the console scope (the mesh task's start, mesh_set_console_scope): "" = not set
void        mesh_cscope_note(char const *name);
// NULL when a send may go on, else MESH_SCOPE_CONSOLE_UNSET: a console line's while the console scope is not set
char const *mesh_cscope_refusal(bool from_console);
// "fw mesh scope" (both scopes) and "fw mesh scope console <region> YES" (sets the console scope)
void        mesh_cscope_console(char const *args);

#ifdef __cplusplus
}
#endif
