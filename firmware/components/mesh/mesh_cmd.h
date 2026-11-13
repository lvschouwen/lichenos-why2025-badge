#pragma once
// The console commands of MeshCore ("fw mesh", "fw contacts", "fw msgs", "fw channels", "fw channel", "fw advert"),
// split out of mesh_task.cpp (#208). They run on the console's task and reach MeshCore only as the screens do: through
// the mesh lock and the copies in mesh_int.h. C++ only and private to the mesh component.

#include "mesh_int.h"

namespace mesh_int {

// ---- mesh_cmd.cpp: the dispatch -------------------------------------------------------------------------------------

bool console_not_running(void);

// ---- mesh_cmd_settings.cpp: name, radio, path hash, advert, location, auto-add, identity ----------------------------

bool arg_onoff(char const *t, bool *v);
void cmd_mesh_name(char const *name);
void cmd_mesh_radio(char const *args);
void cmd_mesh_pathhash(char const *args);
void cmd_mesh_ident_simulate(void);
void cmd_mesh_advert(char const *args);
void cmd_mesh_loc(char const *args);
void cmd_mesh_autoadd(char const *args);

// ---- mesh_cmd_send.cpp: send, dm, path, trace, out, link, chats ---------------------------------------------------

bool console_channel_name(char const *typed, char *out, size_t cap, char const *what);
bool console_contact(char const *hex, uint8_t out[6]);
void cmd_mesh_send(char const *args);
void cmd_mesh_dm(char const *args);
void cmd_mesh_path(char const *args);
void cmd_mesh_trace(char const *arg, mt_kind_t kind);
void cmd_mesh_out(void);
void cmd_mesh_link(void);
void cmd_mesh_chats(void);

// ---- mesh_cmd_remote.cpp: remote, history, telemetry --------------------------------------------------------------

void cmd_mesh_remote(char const *args);
void cmd_mesh_history(char const *args);
void cmd_mesh_telemetry(char const *arg);

// ---- mesh_cmd_store.cpp: the other top-level commands -------------------------------------------------------------

void cmd_contacts(char const *args);
void cmd_msgs(char const *args);
void cmd_channels(char const *args);
void cmd_channel(char const *args);
void cmd_advert(char const *args);

} // namespace mesh_int
