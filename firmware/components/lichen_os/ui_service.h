#pragma once

// Inside lichen_os only: what the debug lock-owner check (ui_lock_assert.c) asks the UI service (lichen_ui_call.c).

#include "ui_lock_owner.h"

// Whether the calling task may touch LVGL now: the LVGL task, or the task inside lichen_sys_ui_lock (#225).
ui_lock_verdict_t ui_service_lock_check(void);
