// The hw object of the OTA server's /info (#321): a few registry keys' cached values as JSON, so a bench run or a
// script sees the hardware tunables without a console. Pure: the text comes in through a callback.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// One key's cached status text (lichen_reg_cached_text's shape): "<value> (<age> ago)" or "? (<why>)", 0 bytes for a
// key the table does not have
typedef size_t (*ota_hw_text_fn)(char const *name, char *buf, size_t cap);

size_t        ota_hw_key_count(void);
char const   *ota_hw_key(size_t i);

// The value inside a cached status text, without the age suffix, as a NUL-terminated string in out. False (the key is
// then JSON null) for an empty text, a "? (...)" one, or a value that does not fit.
bool ota_hw_value(char const *text, char *out, size_t cap);

// {"name":"value",...} for every key of the table; a missing or unread key is null. Always valid JSON and always NUL
// terminated: a key that does not fit is left out together with all after it. Returns the length (0 only for cap < 3).
size_t ota_hw_json(ota_hw_text_fn text, char *buf, size_t cap);
