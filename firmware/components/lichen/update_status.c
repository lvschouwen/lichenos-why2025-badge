#include "update_status.h"

#include <stdio.h>
#include <string.h>

upd_result_t update_result(char const *prev_version, char const *running_version, bool rolled_back) {
    if (!prev_version || !prev_version[0]) {
        return UPD_NONE;
    }
    if (rolled_back) {
        return UPD_ROLLED_BACK;
    }
    return strcmp(prev_version, running_version ? running_version : "") != 0 ? UPD_UPDATED : UPD_NONE;
}

size_t update_line(upd_result_t r, char const *prev, char const *run, char *out, size_t cap) {
    (void)prev;
    if (cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (r == UPD_NONE) {
        return 0;
    }
    int n = snprintf(out, cap, r == UPD_UPDATED ? "Updated to %s" : "Update failed, back on %s", run ? run : "?");
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

int update_pct(uint32_t done, uint32_t total) {
    if (total == 0) {
        return -1;
    }
    if (done >= total) {
        return 100;
    }
    return (int)((uint64_t)done * 100 / total);
}

uint32_t update_stall_limit_ms(bool firmware, uint32_t done, uint32_t total) {
    return firmware && total > 0 && done >= total ? 180000 : 30000;
}

upd_boot_t update_after_boot(char const *prev_version, char const *running_version, bool rolled_back, bool image_valid) {
    upd_boot_t b = {.result = update_result(prev_version, running_version, rolled_back)};
    if (!prev_version || !prev_version[0]) {
        return b; // nothing stored, nothing to erase
    }
    b.erase = b.result == UPD_NONE || b.result == UPD_ROLLED_BACK || image_valid;
    return b;
}
