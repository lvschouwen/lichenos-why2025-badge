#pragma once
// The file calls an SD app gets under their libc names (#533, export_file.c): the OS side of the book of open files.
// The shims themselves are named in tools/export_allow.txt; only the loader needs this header.

#include "lichen_app.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Closes every file ctx still has open (the loader, when the app leaves: before esp_elf_deinit, and when the card goes).
// Slots of a card that is gone or was swapped are only dropped: their descriptors died with the mount and the numbers may
// belong to someone else now. Takes the SD lock (the loader task, never the LVGL task). Returns how many were open.
size_t export_file_close_all(lichen_ctx_t ctx);

#ifdef __cplusplus
}
#endif
