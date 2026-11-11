#pragma once
// About > For developers (#334): turns one registry key's cached status text (lichen_reg_cached_text: "<v> (3.2 s ago)",
// "<v> (stale, 4 s ago)", "? (<why>)", or 0 bytes for a key the table lacks) into the line About shows. Pure, so host-tested.
// Written locally; #332 builds a shared applied/stale helper in app_settings' area, which this should fold into later.
#include <stdbool.h>
#include <stddef.h>

// disp.refresh is in dHz ("446" -> "44.6 Hz"), psram.clk in Hz ("200000000" -> "200 MHz"); other values pass through,
// an empty value reads "none", and the age (and a "stale" mark) always stays. raw == NULL or "" -> "not available".
// Returns the length written (always NUL-terminated when cap > 0).
size_t about_dev_text(char const *key, char const *raw, char *out, size_t cap);
