#include "os_name.h"

#include <stdio.h>

char const *lichen_os_name(void) {
    return "Lichen OS 2";
}

char const *lichen_os_version_line(char const *fw_version, char *buf, size_t cap) {
    if (cap == 0) {
        return buf;
    }
    snprintf(buf, cap, "%s (%s)", lichen_os_name(), fw_version && fw_version[0] ? fw_version : "?");
    return buf;
}
