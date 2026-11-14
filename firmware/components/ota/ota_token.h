#pragma once
// The update token kept by the server (#447): its text rules. ESP-free, host-tested in host_tests/ota_token_test.c.

#include <stdbool.h>

// The first OTA_TOKEN_LEN characters of text are all lowercase hex (what the token file and its NVS copy must hold)
bool ota_token_text_valid(char const *text);
