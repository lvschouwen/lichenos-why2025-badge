// The update token (#447, from ota_server.c): read from /storage, else from its NVS copy, else made new and saved to
// both. ESP-free (the settings through ota_os, randomness through lichen_random), host-tested in
// host_tests/ota_token_test.c. Never logged; "fw token" prints it as a secret line (ota_server.c).
#include "ota_token.h"

#include "lichen_random.h"
#include "ota_os.h"
#include "ota_proto.h"
#include "secure_wipe.h"
#include "storage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef OTA_TOKEN_FILE
#define OTA_TOKEN_FILE STORAGE_BASE "/ota_token.txt" // host tests point it at a scratch file
#endif

static char token[OTA_TOKEN_LEN + 1];

bool ota_token_text_valid(char const *text) {
    for (int i = 0; i < OTA_TOKEN_LEN; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

#define TOKEN_NVS_KEY "ota_token" // the copy that survives an unmounted /storage (#112)

static bool read_token_file(char *out) {
    FILE *f = fopen(OTA_TOKEN_FILE, "r");
    if (!f) {
        return false;
    }
    char text[OTA_TOKEN_LEN + 8] = {0};
    fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[OTA_TOKEN_LEN] = '\0';
    if (!ota_token_text_valid(text)) {
        return false;
    }
    strcpy(out, text);
    return true;
}

static bool read_token_nvs(char *out) {
    char text[OTA_TOKEN_LEN + 8] = {0};
    if (!ota_os_secret_get(TOKEN_NVS_KEY, text, sizeof(text))) {
        return false;
    }
    if (strlen(text) != OTA_TOKEN_LEN || !ota_token_text_valid(text)) {
        return false;
    }
    strcpy(out, text);
    return true;
}

static bool write_token_file(void) {
    FILE *f = fopen(OTA_TOKEN_FILE, "w");
    if (!f) {
        return false;
    }
    bool written = fprintf(f, "%s\n", token) > 0;
    return fclose(f) == 0 && written;
}

// /storage first (what tools read), NVS as the copy that still works when /storage is not mounted. True when the
// token will be the same next boot.
bool ota_token_load(void) {
    bool from_file = read_token_file(token);
    bool from_nvs  = !from_file && read_token_nvs(token);
    if (!from_file && !from_nvs) {
        // The token guards firmware installs: it must not be predictable (lichen_random keeps the entropy source on)
        uint8_t raw[OTA_TOKEN_LEN / 2];
        lichen_random_fill(raw, sizeof(raw));
        ota_hex(raw, sizeof(raw), token);
    }
    bool wrote_file = !from_file && write_token_file();
    bool wrote_nvs  = false;
    char stored[OTA_TOKEN_LEN + 8];
    if (!(from_nvs || (read_token_nvs(stored) && strcmp(stored, token) == 0))) {
        wrote_nvs = ota_os_secret_set(TOKEN_NVS_KEY, token);
    }
    return ota_token_kept(from_file, from_nvs, wrote_file, wrote_nvs);
}

char const *ota_token_get(void) {
    return token;
}

void ota_token_forget(void) {
    secure_wipe(token, sizeof(token));
}
