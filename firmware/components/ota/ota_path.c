#include "ota_path.h"

#include "ota_proto.h"
#include "sdcard.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>

#include <string.h>
#include <strings.h>

#define DRIVE    "FLASH0:"
#define SD_DRIVE "SD0:"

bool ota_path_is_sd(char const *vms) {
    return strncmp(vms, SD_DRIVE, strlen(SD_DRIVE)) == 0;
}

bool ota_path_to_posix(char const *vms, char *out, size_t cap) {
    // ota_path_allowed() checks the syntax: name characters only, no empty directory parts (so no "." or "..")
    if (!ota_path_allowed(vms)) {
        return false;
    }
    bool         sd     = ota_path_is_sd(vms);
    char const  *prefix = sd ? SD_BASE "/" : STORAGE_BASE "/";
    char const  *p      = vms + strlen(sd ? SD_DRIVE : DRIVE);
    size_t       n      = strlen(prefix);
    if (n >= cap) {
        return false;
    }
    memcpy(out, prefix, n);
    if (*p == '[') {
        for (p++; *p != ']'; p++) {
            if (n + 1 >= cap) {
                return false;
            }
            out[n++] = *p == '.' ? '/' : *p;
        }
        p++;
        if (n + 1 >= cap) {
            return false;
        }
        out[n++] = '/';
    }
    size_t rest = strlen(p);
    if (n + rest >= cap) {
        return false;
    }
    memcpy(out + n, p, rest + 1);
    return true;
}

size_t ota_path_next_dir(char const *posix, size_t from) {
    size_t i;
    if (from == 0) {
        char const *root = posix[0] ? strchr(posix + 1, '/') : NULL;
        if (!root) {
            return 0;
        }
        i = (size_t)(root - posix) + 1;
    } else {
        i = from + 1;
    }
    for (; posix[i]; i++) {
        if (posix[i] == '/') {
            return i;
        }
    }
    return 0;
}

bool ota_rename_retry(bool sd, int err) {
    return sd && err == EEXIST;
}

bool ota_place_drops_temp(ota_place_t step) {
    return step != OTA_PLACE_OK && step != OTA_PLACE_LOST;
}

bool ota_sd_missing(char const *vms, bool mounted) {
    return ota_path_is_sd(vms) && !mounted;
}

size_t ota_cluster_bytes(char const *vms) {
    return ota_path_is_sd(vms) ? 32768 : STORAGE_CLUSTER; // FAT32 on a 32 GB card: the card's real value isn't exported
}

bool ota_temp_to_posix(char const *vms, char *out, size_t cap) {
    // Map the real target (ota_path_allowed() refuses the temp name itself), then swap its file name
    char target[OTA_PATH_MAX + sizeof(STORAGE_BASE "/")];
    if (!ota_path_to_posix(vms, target, sizeof(target))) {
        return false;
    }
    char const *slash = strrchr(target, '/');
    int         n     = snprintf(out, cap, "%.*s%s", (int)(slash - target + 1), target, OTA_TEMP_NAME);
    return n > 0 && (size_t)n < cap;
}

// The SD card is held as a whole while the on-air list is on (#528): the card is the user's, a push there is not one
// of the radio files the list protects, but the USB hold refuses every change the list's owner didn't make. "sd" is
// named here (ota_path_onair_held) instead of relying on "not /storage" failing closed.
// The /storage root names the on-air hold covers besides the C6 folder (#454). lora* and ota_* are whole families, the
// same the Files app protects at the root (files_model.c): lora.toml sets what the radio sends, ota_token.txt the next
// boot's update token. meshcore is MeshCore's app root (MESHCORE_STORAGE_ROOT, its identity and keys; ota_path_test
// pins the name).
static char const *const HELD_PREFIX[] = {"lora", "ota_"};
static char const *const HELD_ROOT[]   = {"meshcore"};

static bool held_root_name(char const *name, size_t len) {
    while (len && (name[len - 1] == '.' || name[len - 1] == ' ')) {
        len--; // "lora.toml." and "meshcore " name the same thing on a drive that drops them (fails closed)
    }
    for (size_t i = 0; i < sizeof(HELD_PREFIX) / sizeof(HELD_PREFIX[0]); i++) {
        size_t n = strlen(HELD_PREFIX[i]);
        if (len >= n && strncasecmp(name, HELD_PREFIX[i], n) == 0) {
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(HELD_ROOT) / sizeof(HELD_ROOT[0]); i++) {
        if (strlen(HELD_ROOT[i]) == len && strncasecmp(name, HELD_ROOT[i], len) == 0) {
            return true;
        }
    }
    return false;
}

bool ota_path_onair_held(char const *posix) {
    size_t k = 0; // segments seen
    for (char const *p = posix; *p;) {
        if (*p == '/') {
            p++;
            continue;
        }
        size_t len = strcspn(p, "/");
        if (p[0] == '.' && (len == 1 || (len == 2 && p[1] == '.'))) {
            return true; // "." or "..": where it ends up is not read off the text
        }
        if (k == 0 && len == strlen("sd") && strncasecmp(p, "sd", len) == 0) {
            return true;
        }
        if (k == 0 && !(len == strlen("storage") && strncasecmp(p, "storage", len) == 0)) {
            return true; // ota_path_to_posix only makes /storage paths: anything else is not understood
        }
        if (k == 1 && held_root_name(p, len)) {
            return true;
        }
        k++;
        p += len;
    }
    return false;
}
