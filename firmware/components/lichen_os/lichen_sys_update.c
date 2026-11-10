// lichen_sys_update.h, lichen_sys_crash.h and lichen_sys_factory.h's request (#227) over the ota, crash and lichen_boot
// components, so the shell never links to them. The pure parts (auto-off steps, factory combo) are in lichen_sys_pure.c.
#include "lichen_sys_crash.h"
#include "lichen_sys_factory.h"
#include "lichen_sys_update.h"

#include "crash.h"
#include "factory_reset.h"
#include "ota_server.h"
#include "ota_trial.h"

bool lichen_sys_update_enabled(void) {
    return ota_server_enabled();
}

bool lichen_sys_update_refused(void) {
    return ota_server_refused();
}

void lichen_sys_update_set_enabled(bool on) {
    ota_server_set_enabled(on);
}

uint8_t lichen_sys_update_auto_off(void) {
    return ota_server_auto_off();
}

void lichen_sys_update_set_auto_off(uint8_t minutes) {
    ota_server_set_auto_off(minutes);
}

bool lichen_sys_update_auto_closed(void) {
    return ota_server_auto_closed();
}

bool lichen_sys_update_on_trial(void) {
    return ota_image_trial_pending();
}

bool lichen_sys_update_rolled_back(void) {
    return ota_rolled_back();
}

bool lichen_sys_crash_recovered(void) {
    return crash_recovered();
}

bool lichen_sys_factory_request(lichen_factory_level_t level) {
    return factory_reset_request((factory_level_t)level);
}
