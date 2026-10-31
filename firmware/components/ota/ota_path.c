#include "ota_path.h"

#include "ota_proto.h"

#include <stdio.h>

#include <string.h>

#define DRIVE  "FLASH0:"
#define PREFIX "/storage/"

bool ota_path_to_posix(char const *vms, char *out, size_t cap) {
    // ota_path_allowed() checks the syntax: name characters only, no empty directory parts (so no "." or "..")
    if (strncmp(vms, DRIVE, strlen(DRIVE)) != 0 || !ota_path_allowed(vms)) {
        return false;
    }
    char const *p = vms + strlen(DRIVE);
    size_t      n = strlen(PREFIX);
    if (n >= cap) {
        return false;
    }
    memcpy(out, PREFIX, n);
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

bool ota_temp_to_posix(char const *vms, char *out, size_t cap) {
    // Map the real target (ota_path_allowed() refuses the temp name itself), then swap its file name
    char target[OTA_PATH_MAX + sizeof(PREFIX)];
    if (!ota_path_to_posix(vms, target, sizeof(target))) {
        return false;
    }
    char const *slash = strrchr(target, '/');
    int         n     = snprintf(out, cap, "%.*s%s", (int)(slash - target + 1), target, OTA_TEMP_NAME);
    return n > 0 && (size_t)n < cap;
}
