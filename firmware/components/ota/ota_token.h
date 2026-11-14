#pragma once
// The update token kept by the server (#447): its text rules, where it is kept and how a new one is made. ESP-free,
// host-tested in host_tests/ota_token_test.c.

#include <stdbool.h>
#include <stddef.h>

// The first OTA_TOKEN_LEN characters of text are all lowercase hex (what the token file and its NVS copy must hold)
bool ota_token_text_valid(char const *text);

// /storage first (what tools read), NVS as the copy that still works when /storage is not mounted, else a new random
// one; whichever copy is missing or different is written. True when the token will be the same next boot.
bool ota_token_load(void);
// The server's token: "" before ota_token_load and after ota_token_forget
char const *ota_token_get(void);
// Wipe it (a token kept nowhere, #116): an empty token fails every comparison (ota_token_equal)
void ota_token_forget(void);

// "fw token new" (#456): a new random token saved for the next boot; the running server keeps the one it has until the
// restart. out (OTA_TOKEN_LEN + 1) gets it, wiped again when nothing was saved. SAVED: file and NVS (or the file alone,
// NVS failing: the next boot copies the file to NVS). NVS_ONLY: no token file could be written and none is there.
// FAILED: a token file is there but could not be replaced (it would win at boot), or nothing could be written.
// A cut at any point boots with the old token or the new one, never a mix: the file wins at boot.
typedef enum { OTA_ROTATE_FAILED, OTA_ROTATE_SAVED, OTA_ROTATE_NVS_ONLY } ota_rotate_t;
ota_rotate_t ota_token_rotate(char *out);
// A new token was saved this boot: it is in effect after the restart
bool         ota_token_rotated(void);
