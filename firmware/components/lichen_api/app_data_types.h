#pragma once
// The figures Settings > Apps shows about an app's data (#233); pure, so the page's model and its host test use them
// without LVGL. lichen_sys_app_data.h has the calls that fill them.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     ok;             // the settings and the storage drive could be read (an unreadable SD card is sd_known false)
    uint32_t settings_bytes; // the app's settings namespace in NVS: its entries at 32 bytes each, secrets included
    uint64_t files_bytes;    // its directory on the storage drive
    uint32_t files;          // files there and on the card
    bool     sd_known;       // a card is mounted and its directory was read
    uint64_t sd_bytes;       // its directory on the card
} lichen_app_data_t;

typedef struct {
    bool     settings_ok; // the settings namespace is empty now
    bool     files_ok;    // nothing is left of its directory on the storage drive
    bool     sd_ok;       // nothing is left on the card (true when no card was mounted: there is nothing to clear)
    uint64_t bytes;       // the files that went (settings are counted in entries, not here)
} lichen_app_clear_t;

#ifdef __cplusplus
}
#endif
