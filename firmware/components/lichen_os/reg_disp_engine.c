#include "reg_disp_engine.h"

#include "display_engine.h"
#include "display_info.h"
#include "lichen_sys_ui.h"

// fw_console.h's, declared here: that header pulls in sdkconfig.h, which the host test does not have
bool console_dev_mode(void);

reg_disp_engine_why_t reg_disp_engine_set(uint32_t id, bool dev_build) {
    if (!dev_build) {
        return REG_DISP_ENGINE_RELEASE;
    }
    if (!console_dev_mode()) {
        return REG_DISP_ENGINE_DEV_OFF;
    }
    if (!display_engine_valid(id)) {
        return REG_DISP_ENGINE_BAD;
    }
    if (!lichen_sys_ui_lock(REG_DISP_ENGINE_LOCK_MS)) {
        return REG_DISP_ENGINE_NO_LOCK;
    }
    bool const ok = display_engine_set(id);
    lichen_sys_ui_unlock();
    return ok ? REG_DISP_ENGINE_SET : REG_DISP_ENGINE_REFUSED;
}

char const *reg_disp_engine_why_text(reg_disp_engine_why_t why) {
    switch (why) {
        case REG_DISP_ENGINE_SET: return "set";
        case REG_DISP_ENGINE_RELEASE: return "developer builds only";
        case REG_DISP_ENGINE_DEV_OFF: return "developer mode is off";
        case REG_DISP_ENGINE_BAD: return "not an engine";
        case REG_DISP_ENGINE_NO_LOCK: return "LVGL lock not taken";
        default: return "refused by the display";
    }
}
