#pragma once

#include "boot_steps.h"

#include <stdbool.h>
#include <stdint.h>

// The start-up steps for the rest of the firmware (#66): the same calls as boot_steps.h, safe from any task and from
// the SNTP callback. Readers (the boot screen, "fw boot") go through boot_progress_view(), never boot_steps_view().

void        boot_progress_start(void); // resets the model and adds "fw boot"
void        boot_progress_nvs_up(void); // right after nvs_flash_init: reads the "fw boot fail" flag
// Where that flag (badge_fw "boot_fail") is kept (the settings service, #245): lichen_os binds it in start_nvs, before
// boot_progress_nvs_up. Unbound, no flag is read or saved. Plain pointer types (fw_store.h's), so includers need no
// fw_console.
void        boot_progress_bind_store(uint8_t (*get)(char const *key, uint8_t def),
                                     bool (*set)(char const *key, uint8_t value));
void        boot_progress_begin(boot_step_t step);
void        boot_progress_begin_background(boot_step_t step);
void        boot_progress_progress(boot_step_t step, int pct);
void        boot_progress_end(boot_step_t step, step_state_t state, char const *reason);
void        boot_progress_view(boot_view_t *out);
char const *boot_progress_label(boot_step_t step);
