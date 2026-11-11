// Host-test stubs for the getters/setters that reg_p4_sensors.c points at (#312). Only the sensors agent edits this file.
#include "reg_p4.h"
#include "ui_lock_owner.h"

// Weak, so a sibling domain stubbing the same UI lock call cannot break the link of reg_service_test, which pulls in
// every stub file.
__attribute__((weak)) ui_lock_verdict_t ui_service_lock_check(void) {
    return UI_LOCK_UNARMED; // not the LVGL task: a live ask
}

// No chip on the host: a clear failure
__attribute__((weak)) bool sensors_cfg_get(unsigned key, bool live, uint32_t *out) {
    (void)key;
    (void)live;
    (void)out;
    return false;
}
