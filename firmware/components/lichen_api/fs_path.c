#include "fs_path.h"

#include "app_ns.h"

#include <stdio.h>
#include <string.h>

bool fs_app_dir_path(char const *base, char const *id, char *out, size_t cap) {
    char ns[16];
    if (!base || base[0] != '/' || !app_ns_name(id, ns, sizeof(ns))) {
        return false;
    }
    size_t need = strlen(base) + 1 + strlen(id) + 1;
    if (!out || cap < need) {
        return false;
    }
    snprintf(out, cap, "%s/%s", base, id);
    return true;
}
