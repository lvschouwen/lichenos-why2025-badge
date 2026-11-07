#pragma once
// How a lower-layer component reaches its saved values without opening NVS itself (#235, rule C of #218): the
// settings service (lichen_os) binds these in settings_bind_services() at START_EARLY. Same badge_fw keys as before the
// move. Pure types, no ESP headers.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t (*fw_get_u8_fn)(char const *key, uint8_t def);
typedef bool (*fw_set_u8_fn)(char const *key, uint8_t value);
// A blob: get is true only when exactly size bytes were stored under the key
typedef bool (*fw_get_blob_fn)(char const *key, void *out, size_t size);
typedef bool (*fw_set_blob_fn)(char const *key, void const *data, size_t size);
