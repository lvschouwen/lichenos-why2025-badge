#pragma once

#include "mesh_applied.h"

#ifdef __cplusplus
extern "C" {
#endif

// "fw mesh"'s radio lines (mesh_radio_line.c): MeshCore's radio settings and the region preset they match, then the
// radio the C6 last applied (#335) and whether it differs
void mesh_radio_console(void);

// The applied LoRa radio from the registry cache (lichen_reg_get: never waits, any task)
void mesh_applied_read(mesh_applied_t *out);

#ifdef __cplusplus
}
#endif
