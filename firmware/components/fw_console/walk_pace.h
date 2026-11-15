#pragma once

#include <stdbool.h>
#include <stdint.h>

// Directory walks in bounded slices (#471). A walk over /storage stats every entry, and a LittleFS stat in a folder
// of a few hundred files costs milliseconds (the lookup reads the folder's metadata from flash): a walk on a pinned
// task above idle priority never blocks, so IDLE on that core starves and the task watchdog fires after 5 s ("fw
// storage meshcore/bl" over Wi-Fi on the ota task, core 0, priority 5). The walker calls walk_pace() after every
// entry; once the stretch since the last block reaches WALK_SLICE_US it blocks for a tick, so every task on the core,
// IDLE included, gets to run. Never call it with the LVGL lock held; under a lock others wait on (the SD session of
// an app's data walk) each block lengthens their wait by a tick.

#define WALK_SLICE_US 20000 // one stretch: well under the 100 ms the issue allows, ~5% more total time at a 1 ms tick

typedef struct {
    int64_t  start_us; // when the current stretch began
    uint32_t worst_us; // the longest stretch of this walk so far
    uint32_t yields;   // the blocks so far
} walk_slice_t;

// The pure rule (host_tests/walk_pace_test.c): begin before the first entry, step after each one with the time now.
// Step records the stretch and is true when the walk must block before the next entry; resume after the block.
void walk_slice_begin(walk_slice_t *s, int64_t now_us);
bool walk_slice_step(walk_slice_t *s, int64_t now_us);
void walk_slice_resume(walk_slice_t *s, int64_t now_us);

// The badge's glue (walk_pace.c): the same with esp_timer and a one-tick vTaskDelay. walk_pace_end folds the walk's
// longest stretch into walk_pace_worst_ms, the since-boot worst over every walk ("fw soak" wkmax).
void     walk_pace_begin(walk_slice_t *s);
void     walk_pace(walk_slice_t *s);
void     walk_pace_end(walk_slice_t *s);
uint32_t walk_pace_worst_ms(void);
