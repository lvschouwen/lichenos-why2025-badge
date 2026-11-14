#pragma once
// Hex text, one implementation (#445). Pure, host-tested in host_tests/lichen_hex_test.c. It parses untrusted text (keys
// typed on the console, ids and names from the air), so every edge is pinned there: either case is read, nothing else
// is a digit, and a decode writes nothing unless the whole text is good.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0..15 for '0'-'9', 'a'-'f' and 'A'-'F', -1 for every other char (NUL, space, 'g', a byte >= 0x80 included).
int lichen_hex_digit(char c);

// Exactly `bytes` bytes from exactly 2 * bytes digits at text (len chars, not necessarily NUL-terminated). False, out
// untouched, for any other len (odd, short, long) or any non-hex char. bytes == 0 is true for len == 0.
bool lichen_hex_decode(char const *text, size_t len, uint8_t *out, size_t bytes);

// n bytes as 2 * n lower-case digits plus a NUL. False when out cannot hold 2 * n + 1 chars (out[0] = '\0' when cap > 0,
// so the text is never half there); cap == 0 writes nothing.
bool lichen_hex_encode(uint8_t const *bytes, size_t n, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
