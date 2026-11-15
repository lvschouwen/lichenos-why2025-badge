#pragma once

#include <stdbool.h>
#include <stdint.h>

// disp.engine's set path (#468), the body of reg_p4_disp_engine_set without the log, so a host test runs it with
// console_dev_mode, the UI lock and display_engine_set stubbed (host_tests/reg_disp_engine_test.c). A bench switch:
// developer builds and developer mode only, the switch under the UI service's LVGL lock (no refresh runs, so it lands
// between two frames).
typedef enum {
    REG_DISP_ENGINE_SET,     // applied (the read-back says which)
    REG_DISP_ENGINE_RELEASE, // a release image has no PPA engine
    REG_DISP_ENGINE_DEV_OFF, // developer mode is off
    REG_DISP_ENGINE_BAD,     // not an engine id
    REG_DISP_ENGINE_NO_LOCK, // the LVGL lock was not free in time
    REG_DISP_ENGINE_REFUSED, // the display said no (no direct flush, or the PPA is still busy after a timeout)
} reg_disp_engine_why_t;

#define REG_DISP_ENGINE_LOCK_MS 2000 // as the UI service's calls: a refresh takes milliseconds

reg_disp_engine_why_t reg_disp_engine_set(uint32_t id, bool dev_build);
char const           *reg_disp_engine_why_text(reg_disp_engine_why_t why);
