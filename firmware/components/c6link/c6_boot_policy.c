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

// The first reason this boot must check the C6 fully, NULL when nothing stands in the way of a skip. The order is the
// log's: the first that applies is the one reported.
static char const *full_check_reason(c6_reset_kind_t kind, bool force, bool verify_flag, bool trial_image,
                                     char const *stored_key, char const *current_key) {
    if (kind != C6_RST_SOFTWARE) {
        return "not a software reset";
    }
    if (force) {
        return "forced reflash";
    }
    if (verify_flag) {
        return "verify flag (last attach failed)";
    }
    if (trial_image) {
        return "trial image (a restart would roll it back)";
    }
    if (!current_key || !current_key[0]) {
        return "no readable bundle";
    }
    if (!stored_key || !stored_key[0]) {
        return "no verified bundle stored";
    }
    return strcmp(stored_key, current_key) != 0 ? "bundle changed" : NULL;
}

c6_plan_t c6_boot_plan(c6_reset_kind_t kind, bool force, bool verify_flag, bool trial_image, char const *stored_key,
                       char const *current_key, char const **why) {
    char const *reason = full_check_reason(kind, force, verify_flag, trial_image, stored_key, current_key);
    c6_plan_t   plan   = reason ? C6_PLAN_FULL_CHECK : C6_PLAN_SKIP;
    if (why) {
        *why = reason ? reason : "bundle unchanged since the last verify";
    }
    return plan;
}

c6_ident_t c6_ident_check(uint8_t const *running, uint8_t const *expected) {
    if (!running) {
        return C6_IDENT_NO_ANSWER;
    }
    if (!expected) {
        return C6_IDENT_NO_EXPECTED;
    }
    return memcmp(running, expected, C6_IDENT_SHA_LEN) == 0 ? C6_IDENT_MATCH : C6_IDENT_MISMATCH;
}

char const *c6_ident_name(c6_ident_t r) {
    switch (r) {
        case C6_IDENT_NOT_ASKED: return "not asked";
        case C6_IDENT_MATCH: return "match";
        case C6_IDENT_MISMATCH: return "mismatch";
        case C6_IDENT_NO_ANSWER: return "no answer";
        case C6_IDENT_NO_EXPECTED: return "bundle app unreadable";
    }
    return "?";
}

c6_state_write_t c6_state_after_attach(bool full_path, bool attached, bool ident_ok, bool image_ok, bool key_valid) {
    c6_state_write_t w = {0};
    if (!attached || (!full_path && !ident_ok)) {
        w.write  = true; // key erased, verify set: the next boot checks fully
        w.verify = true;
    } else if (full_path) {
        w.write     = true;
        w.store_key = image_ok && key_valid && ident_ok;
    }
    return w;
}

bool c6_bundle_pending(bool changed, bool checked, bool staged, bool ident_ok) {
    return changed || !checked || (staged && !ident_ok);
}

bool c6_restart_after_attach(bool full_path, bool attached, bool ident_ok, bool state_persisted) {
    return !full_path && (!attached || !ident_ok) && state_persisted;
}

uint32_t c6_sdio_raise_khz(uint32_t configured_khz, int applied_khz) {
    return applied_khz == C6_SDIO_HS_CAP_KHZ && configured_khz > C6_SDIO_HS_CAP_KHZ ? configured_khz : 0;
}

c6_card_t c6_card_classify(unsigned files, char const *card_key, char const *storage_key, bool failed_before) {
    if (files == 0) {
        return C6_CARD_NONE;
    }
    if (files < 6 || !card_key || !card_key[0]) {
        return C6_CARD_HALF;
    }
    if (storage_key && storage_key[0] && strcmp(card_key, storage_key) == 0) {
        return C6_CARD_SAME;
    }
    return failed_before ? C6_CARD_FAILED : C6_CARD_NEW;
}

char const *c6_card_name(c6_card_t c) {
    switch (c) {
    case C6_CARD_NONE: return "none";
    case C6_CARD_HALF: return "incomplete";
    case C6_CARD_SAME: return "same as /storage";
    case C6_CARD_FAILED: return "failed before";
    case C6_CARD_NEW: return "new";
    default: return "?";
    }
}

bool c6_card_use(c6_card_t c, bool md5_ok) {
    return c == C6_CARD_NEW && md5_ok;
}

bool c6_card_record(char const *card_key, uint8_t const p4_sha[C6_IDENT_SHA_LEN], char *out, size_t cap) {
    if (!out || !cap) {
        return false;
    }
    out[0] = '\0';
    if (!card_key || !card_key[0] || !p4_sha) {
        return false;
    }
    int n = snprintf(out, cap, "%s|", card_key);
    if (n <= 0 || (size_t)n + 2 * C6_IDENT_SHA_LEN >= cap) {
        out[0] = '\0';
        return false;
    }
    for (int i = 0; i < C6_IDENT_SHA_LEN; i++) {
        snprintf(out + n + 2 * i, 3, "%02x", p4_sha[i]);
    }
    return true;
}

c6_card_after_t c6_card_after(bool from_card, bool on_c6, bool ident_ok, bool p4_trial) {
    if (!from_card) {
        return C6_CARD_KEEP;
    }
    if (ident_ok) {
        return p4_trial ? C6_CARD_KEEP : C6_CARD_COPY;
    }
    return on_c6 ? C6_CARD_RECORD_FAILED : C6_CARD_KEEP;
}

bool c6_card_copy_fits(bool free_known, uint64_t free_bytes, unsigned long const bin_sizes[3]) {
    if (!free_known) {
        return true;
    }
    uint64_t blocks = C6_CARD_COPY_SLACK + 3 * 2; // one metadata block per .bin and .md5, the .md5 data inline
    for (int i = 0; i < 3; i++) {
        blocks += ((uint64_t)bin_sizes[i] + 4087) / 4088;
    }
    return blocks * 4096 <= free_bytes;
}
