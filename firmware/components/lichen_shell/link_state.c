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
    return (link_look_t){have_credentials ? LINK_DOWN : LINK_OFF, 0, THEME_LINK_RED};
}

bool wifi_fan_part_lit(int bars, int part) {
    return part >= 0 && part < WIFI_FAN_PARTS && part < bars;
}

link_look_t lora_look(bool up, bool wanted) {
    if (!wanted) {
        return (link_look_t){LINK_OFF, 0, THEME_DIM}; // nobody uses the radio by choice (#103): not an alarm
    }
    return (link_look_t){up ? LINK_UP : LINK_DOWN, 0, up ? THEME_TEAL : THEME_LINK_RED};
}
