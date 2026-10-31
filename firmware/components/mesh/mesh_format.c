#include "mesh_format.h"

#include <stdio.h>

void mesh_format_age(uint32_t age_s, char *out, size_t cap) {
    if (age_s < 5) {
        snprintf(out, cap, "just now");
    } else if (age_s < 60) {
        snprintf(out, cap, "%lu s ago", (unsigned long)age_s);
    } else if (age_s < 3600) {
        snprintf(out, cap, "%lu min ago", (unsigned long)(age_s / 60));
    } else if (age_s < 86400) {
        snprintf(out, cap, "%lu h ago", (unsigned long)(age_s / 3600));
    } else {
        snprintf(out, cap, "%lu d ago", (unsigned long)(age_s / 86400));
    }
}

char const *mesh_type_name(uint8_t type) {
    switch (type) {
        case 1: return "chat";
        case 2: return "repeater";
        case 3: return "room";
        case 4: return "sensor";
        default: return "?";
    }
}

bool mesh_name_clean(char const *in, char *out, size_t cap) {
    if (cap == 0) {
        return false;
    }
    size_t n = 0;
    while (*in == ' ') {
        in++;
    }
    for (; *in && n < cap - 1; in++) {
        if ((unsigned char)*in >= 0x20 && *in != 0x7F) {
            out[n++] = *in;
        }
    }
    // A multi-byte character cut short at the end goes whole: back to the byte that starts it
    if (*in && n > 0 && ((unsigned char)out[n - 1] & 0x80)) {
        size_t start = n - 1;
        while (start > 0 && ((unsigned char)out[start] & 0xC0) == 0x80) {
            start--;
        }
        unsigned char lead = (unsigned char)out[start];
        size_t        len  = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (start + len > n) {
            n = start;
        }
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
    return n > 0;
}
