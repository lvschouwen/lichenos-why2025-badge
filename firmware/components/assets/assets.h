// The "assets" partition (#22): read-only files such as fonts, memory-mapped at boot. Missing or damaged assets
// are not fatal: assets_get() then finds nothing and callers fall back to what the firmware has built in.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ASSETS_PARTITION "assets"

// Map and check the partition; false (with a log line) when there is no usable image
bool assets_mount(void);
// A file by name; false when assets aren't mounted or the file isn't there. The data is the memory-mapped partition
// itself: copy what you keep (e.g. load a font into RAM at startup) and don't read it later, because a Wi-Fi assets
// update rewrites the partition under a running firmware (it stops the apps first and reboots afterwards).
bool assets_get(char const *name, uint8_t const **data, uint32_t *size);
// One line for the console and the About screen, e.g. "3 files, 412 KB" or "damaged (CRC)"
char const *assets_status(void);
