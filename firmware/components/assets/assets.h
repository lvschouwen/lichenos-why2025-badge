// The "assets" partition (#22): read-only files such as fonts, copied into PSRAM at boot until the fonts are loaded.
// Missing or damaged assets are not fatal: assets_get() then finds nothing and callers fall back to what the firmware
// has built in.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ASSETS_PARTITION "assets"

// Read and check the partition; false (with a log line) when there is no usable image
bool assets_mount(void);
// Free the copy once the fonts are loaded; assets_get() finds nothing afterwards. Never mmap the partition: with XIP
// from PSRAM a flash write keeps the cache and interrupts on only while no flash is mapped (#31).
void assets_unmount(void);
// A file by name; false when assets aren't mounted or the file isn't there. The data is the boot-time
// PSRAM copy: copy what you keep (e.g. load a font into RAM at startup) before assets_unmount() frees it.
bool assets_get(char const *name, uint8_t const **data, uint32_t *size);
// One line for the console and the About screen, e.g. "3 files, 412 KB" or "damaged (CRC)"
char const *assets_status(void);
