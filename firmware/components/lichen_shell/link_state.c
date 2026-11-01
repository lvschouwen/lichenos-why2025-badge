#include "link_state.h"

#include "theme.h"
#include "wifi_list.h"

link_look_t wifi_look(bool have_credentials, bool joining, bool has_ip, bool rssi_known, int8_t rssi) {
    if (has_ip) {
        // Connected but no reading yet (the first one comes within 5 s): one bar rather than none, which would read as down
        return (link_look_t){LINK_UP, rssi_known ? wifi_bars(rssi) : 1, THEME_TEAL};
    }
    if (joining) {
        return (link_look_t){LINK_CONNECTING, 0, THEME_AMBER};
    }
    if (!have_credentials) {
        return (link_look_t){LINK_OFF, 0, THEME_DIM}; // off by choice (#105): a faint fan, not an alarm
    }
    return (link_look_t){LINK_DOWN, 0, THEME_LINK_RED};
}

bool wifi_fan_part_lit(int bars, int part) {
    return part >= 0 && part < WIFI_FAN_PARTS && part < bars;
}

link_look_t lora_look(bool up, lora_use_t use) {
    if (use == LORA_USE_OFF) {
        return (link_look_t){LINK_OFF, 0, THEME_DIM}; // nobody uses the radio by choice (#103, #105): not an alarm
    }
    if (!up || use == LORA_USE_FAILED) {
        return (link_look_t){LINK_DOWN, 0, THEME_LINK_RED}; // a fault: the radio or the C6 down, or its user is
    }
    if (use == LORA_USE_CHANGING) {
        return (link_look_t){LINK_CONNECTING, 0, THEME_AMBER};
    }
    return (link_look_t){LINK_UP, 0, THEME_TEAL};
}
