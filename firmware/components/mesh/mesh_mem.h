#pragma once
// MeshCore's memory through the OS books (#228): everything is booked under the MeshCore app, so "fw mem" shows it.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A chat ring: zeroed, PSRAM explicitly (through plain malloc a ring under SPIRAM_MALLOC_ALWAYSINTERNAL would take the
// internal RAM the display needs, L6 of #59)
void *mesh_mem_ring_alloc(size_t count, size_t size);
// A "w" file's buffer for the file shim: PSRAM, and internal RAM (under the app's quota) when PSRAM is full, so a
// small prefs or identity save still lands
void *mesh_mem_file_alloc(size_t bytes);
// Gives either back
void mesh_mem_free(void *ptr);

#ifdef __cplusplus
}
#endif
