// The update token (#447, from ota_server.c): read from /storage, else from its NVS copy, else made new and saved to
// both. ESP-free (the settings through ota_os, randomness through lichen_random), host-tested in
// host_tests/ota_token_test.c. Never logged; "fw token" prints it as a secret line (ota_server.c).
#include "ota_token.h"

#include "lichen_random.h"
#include "ota_os.h"
#include "ota_proto.h"
#include "secure_wipe.h"
#include "storage.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef OTA_TOKEN_FILE
#define OTA_TOKEN_FILE STORAGE_BASE "/ota_token.txt" // host tests point it at a scratch file
#endif
#define OTA_TOKEN_NEW OTA_TOKEN_FILE ".new" // a rotation's new file before it is swapped in; never read at boot

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
    setvbuf(f, NULL, _IONBF, 0); // no stdio buffer: it would keep a copy of the token in freed heap
    char text[OTA_TOKEN_LEN + 8] = {0};
    fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[OTA_TOKEN_LEN] = '\0';
    bool valid          = ota_token_text_valid(text);
    if (valid) {
        strcpy(out, text);
    }
    secure_wipe(text, sizeof(text)); // the token's text, valid or near enough, stays nowhere but in token
    return valid;
}

static bool read_token_nvs(char *out) {
    char text[OTA_TOKEN_LEN + 8] = {0};
    bool valid = ota_os_secret_get(TOKEN_NVS_KEY, text, sizeof(text)) && strlen(text) == OTA_TOKEN_LEN &&
                 ota_token_text_valid(text);
    if (valid) {
        strcpy(out, text);
    }
    secure_wipe(text, sizeof(text)); // a failed read may still have left text behind
    return valid;
}

// Unbuffered, in one fwrite (no fprintf: unbuffered, newlib formats through a stack buffer of BUFSIZ)
static bool write_text(char const *path, char const *text) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    setvbuf(f, NULL, _IONBF, 0); // no stdio buffer: it would keep a copy of the token in freed heap
    char line[OTA_TOKEN_LEN + 2];
    int  n       = snprintf(line, sizeof(line), "%s\n", text);
    bool written = n > 0 && (size_t)n < sizeof(line) && fwrite(line, 1, (size_t)n, f) == (size_t)n;
    secure_wipe(line, sizeof(line));
    return fclose(f) == 0 && written;
}

static bool write_token_file(void) {
    return write_text(OTA_TOKEN_FILE, token);
}

// The token guards firmware installs: it must not be predictable (lichen_random keeps the entropy source on)
static void make_token(char *out) {
    uint8_t raw[OTA_TOKEN_LEN / 2];
    lichen_random_fill(raw, sizeof(raw));
    ota_hex(raw, sizeof(raw), out);
    secure_wipe(raw, sizeof(raw)); // the token itself, as bytes
}

// /storage first (what tools read), NVS as the copy that still works when /storage is not mounted. True when the
// token will be the same next boot.
bool ota_token_load(void) {
    unlink(OTA_TOKEN_NEW); // a rotation cut before its swap: an unused token, never read (and not left on the drive)
    bool from_file = read_token_file(token);
    bool from_nvs  = !from_file && read_token_nvs(token);
    if (!from_file && !from_nvs) {
        make_token(token);
    }
    bool wrote_file = !from_file && write_token_file();
    bool wrote_nvs  = false;
    char stored[OTA_TOKEN_LEN + 8];
    if (!(from_nvs || (read_token_nvs(stored) && strcmp(stored, token) == 0))) {
        wrote_nvs = ota_os_secret_set(TOKEN_NVS_KEY, token);
    }
    secure_wipe(stored, sizeof(stored));
    return ota_token_kept(from_file, from_nvs, wrote_file, wrote_nvs);
}

char const *ota_token_get(void) {
    return token;
}

void ota_token_forget(void) {
    secure_wipe(token, sizeof(token));
}

static bool s_rotated; // console task only

// The file is the authority at boot (ota_token_load), so it is swapped first and in one step: written next to it, then
// renamed over it (LittleFS: a cut leaves the old file or the new one). The NVS copy follows; a cut before that leaves
// the new file and the old NVS copy, and the next boot's load writes the file's token to NVS.
// Whether OTA_TOKEN_FILE is surely not there: only ENOENT says so (a file that can't be opened may still win at boot)
static bool token_file_absent(void) {
    FILE *f = fopen(OTA_TOKEN_FILE, "r");
    if (f) {
        fclose(f);
        return false;
    }
    return errno == ENOENT;
}

ota_rotate_t ota_token_rotate(char *out, bool storage_up) {
    if (!storage_up) { // an unmounted drive may still hold the old file, which wins once it mounts again
        out[0] = '\0';
        return OTA_ROTATE_FAILED;
    }
    make_token(out);
    bool swapped = write_text(OTA_TOKEN_NEW, out) && rename(OTA_TOKEN_NEW, OTA_TOKEN_FILE) == 0;
    if (!swapped) {
        unlink(OTA_TOKEN_NEW);
        if (!token_file_absent()) { // it would bring the old token back at boot, whatever NVS says
            secure_wipe(out, OTA_TOKEN_LEN + 1);
            return OTA_ROTATE_FAILED;
        }
    }
    bool saved = ota_os_secret_set(TOKEN_NVS_KEY, out);
    if (!swapped && !saved) {
        secure_wipe(out, OTA_TOKEN_LEN + 1);
        return OTA_ROTATE_FAILED;
    }
    s_rotated = true;
    return swapped ? OTA_ROTATE_SAVED : OTA_ROTATE_NVS_ONLY;
}

bool ota_token_rotated(void) {
    return s_rotated;
}
