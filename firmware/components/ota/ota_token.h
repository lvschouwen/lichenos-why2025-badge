#pragma once
// The update token kept by the server (#447): its text rules, where it is kept and how a new one is made. ESP-free,
// host-tested in host_tests/ota_token_test.c.

#include <stdbool.h>

// The first OTA_TOKEN_LEN characters of text are all lowercase hex (what the token file and its NVS copy must hold)
bool ota_token_text_valid(char const *text);

// /storage first (what tools read), NVS as the copy that still works when /storage is not mounted, else a new random
// one; whichever copy is missing or different is written. True when the token will be the same next boot.
bool ota_token_load(void);
// The server's token: "" before ota_token_load and after ota_token_forget
char const *ota_token_get(void);
// Wipe it (a token kept nowhere, #116): an empty token fails every comparison (ota_token_equal)
void ota_token_forget(void);
