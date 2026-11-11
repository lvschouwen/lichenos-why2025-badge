#include "ota_hw.h"

#include <stdio.h>
#include <string.h>

// All public hardware tunables; none is a secret (no key whose name holds token, key, pass or secret). A name the
// registry does not have (yet) comes out as null, so the table can run ahead of the keys.
static char const *const keys[] = {
    "psram.clk",   "psram.want",   "psram.dl_idx", "disp.pclk",  "disp.refresh", "sens.acc_odr",
    "sens.gyr_odr", "sens.heat_temp", "haptics.duty", "tz.zone",   "hold.gpio12",  "bl.display",
    "bl.keyboard", "lora.freq",    "lora.power",   "wifi.country", "bt.state",     "settings.write_fail",
};

#define VALUE_MAX 48

size_t ota_hw_key_count(void) {
    return sizeof(keys) / sizeof(keys[0]);
}

char const *ota_hw_key(size_t i) {
    return i < ota_hw_key_count() ? keys[i] : NULL;
}

bool ota_hw_value(char const *text, char *out, size_t cap) {
    size_t len = strlen(text);
    if (!len || text[0] == '?' || !cap) {
        return false;
    }
    // The age suffix is the last " (" of a text that ends in " ago)"
    if (len >= 6 && strcmp(text + len - 5, " ago)") == 0) {
        for (size_t i = len - 5; i-- > 0;) {
            if (text[i] == ' ' && text[i + 1] == '(') {
                len = i;
                break;
            }
        }
    }
    if (!len || len >= cap) {
        return false;
    }
    memcpy(out, text, len);
    out[len] = 0;
    return true;
}

// Appends s as a JSON string (quotes, backslash and control characters escaped); false when it does not fit
static bool put_string(char *buf, size_t cap, size_t *pos, char const *s) {
    size_t p = *pos;
    if (p + 1 >= cap) {
        return false;
    }
    buf[p++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char          esc[7];
        size_t        n;
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            n      = 2;
        } else if (c < 0x20 || c == 0x7F) {
            n = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", c);
        } else {
            esc[0] = (char)c;
            n      = 1;
        }
        if (p + n + 1 >= cap) { // room for the closing quote
            return false;
        }
        memcpy(buf + p, esc, n);
        p += n;
    }
    buf[p++] = '"';
    *pos     = p;
    return true;
}

static bool put_raw(char *buf, size_t cap, size_t *pos, char const *s) {
    size_t n = strlen(s);
    if (*pos + n >= cap) {
        return false;
    }
    memcpy(buf + *pos, s, n + 1);
    *pos += n;
    return true;
}

// One "name":value member, with the comma before it when it is not the first; the buffer is left as it was on failure
static bool put_member(char *buf, size_t cap, size_t *pos, size_t i, char const *name, char const *value) {
    size_t p = *pos;
    // Keep one byte for the closing brace
    size_t room = cap - 1;
    if ((i && !put_raw(buf, room, &p, ",")) || !put_string(buf, room, &p, name) || !put_raw(buf, room, &p, ":") ||
        (value ? !put_string(buf, room, &p, value) : !put_raw(buf, room, &p, "null"))) {
        return false;
    }
    *pos = p;
    return true;
}

size_t ota_hw_json(ota_hw_text_fn text, char *buf, size_t cap) {
    if (cap < 3) {
        if (cap) {
            buf[0] = 0;
        }
        return 0;
    }
    size_t pos = 0;
    buf[pos++] = '{';
    size_t members = 0;
    for (size_t i = 0; i < ota_hw_key_count(); i++) {
        char raw[VALUE_MAX + 24] = "";
        char val[VALUE_MAX];
        if (text && text(keys[i], raw, sizeof(raw)) >= sizeof(raw)) {
            raw[0] = 0;
        }
        bool ok = ota_hw_value(raw, val, sizeof(val));
        if (!put_member(buf, cap, &pos, members, keys[i], ok ? val : NULL)) {
            break;
        }
        members++;
    }
    buf[pos++] = '}';
    buf[pos]   = 0;
    return pos;
}
