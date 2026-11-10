#include "fs_path.h"

#include "app_ns.h"

#include <stdio.h>
#include <string.h>

static char const *const RESERVED_IDS[] = {"c6", "lichen"};

static bool id_reserved(char const *id) {
    for (size_t i = 0; id && i < sizeof(RESERVED_IDS) / sizeof(RESERVED_IDS[0]); i++) {
        if (strcmp(id, RESERVED_IDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool fs_app_dir_path(char const *base, char const *id, char *out, size_t cap) {
    char ns[16];
    if (!base || base[0] != '/' || id_reserved(id) || !app_ns_name(id, ns, sizeof(ns))) {
        return false;
    }
    size_t need = strlen(base) + 1 + strlen(id) + 1;
    if (!out || cap < need) {
        return false;
    }
    snprintf(out, cap, "%s/%s", base, id);
    return true;
}
