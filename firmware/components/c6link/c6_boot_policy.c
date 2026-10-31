#include "c6_boot_policy.h"

#include <stdio.h>
#include <string.h>

static bool copy_md5(char const *md5, char *out) {
    if (!md5) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        char c = md5[i];
        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false; // also catches a string shorter than 32
        }
        out[i] = c;
    }
    out[32] = '\0';
    return true;
}

bool c6_bundle_key(char const *boot_md5, char const *part_md5, char const *app_md5, unsigned long boot_size,
                   unsigned long part_size, unsigned long app_size, char *out, size_t cap) {
    char b[33], p[33], a[33];
    if (!out || !copy_md5(boot_md5, b) || !copy_md5(part_md5, p) || !copy_md5(app_md5, a)) {
        return false;
    }
    int n = snprintf(out, cap, "%s,%s,%s,%lu,%lu,%lu", b, p, a, boot_size, part_size, app_size);
    if (n <= 0 || (size_t)n >= cap) {
        if (cap) {
            out[0] = '\0';
        }
        return false;
    }
    return true;
}

c6_plan_t c6_boot_plan(c6_reset_kind_t kind, bool force, bool verify_flag, char const *stored_key, char const *current_key,
                       char const **why) {
    char const *reason;
    c6_plan_t   plan = C6_PLAN_FULL_CHECK;
    if (kind != C6_RST_SOFTWARE) {
        reason = "not a software reset";
    } else if (force) {
        reason = "forced reflash";
    } else if (verify_flag) {
        reason = "verify flag (last attach failed)";
    } else if (!current_key || !current_key[0]) {
        reason = "no readable bundle";
    } else if (!stored_key || !stored_key[0]) {
        reason = "no verified bundle stored";
    } else if (strcmp(stored_key, current_key) != 0) {
        reason = "bundle changed";
    } else {
        reason = "bundle unchanged since the last verify";
        plan   = C6_PLAN_SKIP;
    }
    if (why) {
        *why = reason;
    }
    return plan;
}
