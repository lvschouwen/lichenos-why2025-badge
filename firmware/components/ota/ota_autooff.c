// See ota_autooff.h
#include "ota_autooff.h"

#include <stdio.h>
#include <string.h>

static uint8_t const choices[] = {0, 10, 30, 60, 120};

uint8_t ota_autooff_next(uint8_t minutes) {
    for (size_t i = 0; i < sizeof(choices); i++) {
        if (choices[i] > minutes) {
            return choices[i];
        }
    }
    return 0;
}

void ota_autooff_text(uint8_t minutes, char *out, size_t cap) {
    if (minutes == 0) {
        snprintf(out, cap, "Never");
    } else if (minutes % 60 == 0) {
        snprintf(out, cap, "%u h", (unsigned)(minutes / 60));
    } else {
        snprintf(out, cap, "%u min", (unsigned)minutes);
    }
}

bool ota_autooff_parse(char const *arg, uint8_t *minutes) {
    if (strcmp(arg, "off") == 0 || strcmp(arg, "never") == 0) {
        *minutes = 0;
        return true;
    }
    unsigned v = 0;
    size_t   n = 0;
    for (; arg[n]; n++) {
        if (arg[n] < '0' || arg[n] > '9' || n >= 3) {
            return false;
        }
        v = v * 10 + (unsigned)(arg[n] - '0');
    }
    if (n == 0 || v == 0 || v > OTA_AUTOOFF_MAX_MIN) {
        return false;
    }
    *minutes = (uint8_t)v;
    return true;
}

uint32_t ota_autooff_left(uint8_t minutes, uint32_t armed_s, uint32_t now_s) {
    if (minutes == 0) {
        return 0;
    }
    uint32_t span    = (uint32_t)minutes * 60u;
    uint32_t elapsed = now_s >= armed_s ? now_s - armed_s : 0; // a clock before the arming: nothing has passed
    return elapsed >= span ? 0 : span - elapsed;
}

bool ota_autooff_due(uint8_t minutes, uint32_t armed_s, uint32_t now_s, bool busy) {
    return minutes != 0 && !busy && ota_autooff_left(minutes, armed_s, now_s) == 0;
}
