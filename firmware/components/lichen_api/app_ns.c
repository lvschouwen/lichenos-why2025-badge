#include "app_ns.h"

#include <stdio.h>
#include <string.h>

#define APP_ID_MAX 13 // "a." + id fits NVS's 15-character namespace

bool app_ns_name(char const *id, char *out, size_t cap) {
    if (!id || cap < APP_ID_MAX + 3) {
        return false;
    }
    size_t n = strlen(id);
    if (n == 0 || n > APP_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    snprintf(out, cap, "a.%s", id);
    return true;
}

app_adopt_t app_adopt_plan(bool have_app, bool have_legacy) {
    return !have_app && have_legacy ? APP_ADOPT_COPY : APP_ADOPT_NONE;
}
