#pragma once

#include <stdbool.h>
#include <stdint.h>

// How the status bar draws a link (#66, Task 9d), pure: colour and filled bars carry the state, no words
typedef enum { LINK_UP, LINK_CONNECTING, LINK_DOWN, LINK_OFF } link_t;

typedef struct {
    link_t   state;
    int      bars;  // filled bars 0-4; the rest are drawn as outlines
    uint32_t color; // teal up, amber joining, red down
} link_look_t;

// Up: bars from the RSSI (1 while it is unknown), teal. Joining: amber, 0 bars. Credentials but no IP: red, 0 bars
// (DOWN). No credentials: red, 0 bars (OFF).
link_look_t wifi_look(bool have_credentials, bool joining, bool has_ip, bool rssi_known, int8_t rssi);
// The "LoRa" label: teal up, red down
link_look_t lora_look(bool up);
