#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The PSRAM speed switch (#121), the part that touches NVS, the LP register and the restart; the rules are in
// psram_speed_logic.c. Needs the patched IDF (tools/idf-patches/0003, CONFIG_SPIRAM_SPEED_RUNTIME_SELECT): without it
// everything here reports 80 MHz, not supported, and never restarts.

// True when the IDF can run 120 MHz
bool psram_speed_supported(void);
// The speed PSRAM init chose this boot, MHz
unsigned psram_speed_running(void);
// The speed the record asks for (80 or 120; 80 again after a failed trial), MHz. One byte read: any task, no lock
unsigned psram_speed_want(void);
// What LP_STORE15 held when PSRAM init read (and cleared) it, for the boot log and "fw psram carrier"
uint32_t psram_speed_carrier_raw(void);

// app_main, once NVS is up: reads the record, applies the rules (a trial that never finished = failed, back at 80; an
// armed 120 wanted at 80 = arm and restart once), registers the shutdown hook that keeps a clean restart at 120 MHz.
// image_on_trial answers whether the running image may still be on trial (ota_image_trial_maybe, #285): no restart
// for the speed while it may be, as that would roll the image back. NULL counts as on trial.
void psram_speed_boot(bool (*image_on_trial)(void));

// Guard task, every ~100 ms: supervises a live 120 MHz trial (after PSRAM_SPEED_TRIAL_SECONDS with a clean boot check a
// job task runs one DMA gate round, then passes it; a bad boot check or gate fails it) and starts the arm / leave that had
// to wait for the image to be valid. Quick: the work runs on the trial job task, not here.
void psram_speed_tick(void);

// A 120 MHz trial is running (MeshCore does not start meanwhile: nothing may be written over a bus that is on trial)
bool psram_speed_trial_pending(void);
// Called once when the trial passes (on the guard task); MeshCore starts itself then. One callback.
void psram_speed_on_pass(void (*cb)(void));

// "fw psram dma" result: a failed gate at 120 MHz is a failed trial, back at 80 (restarts the badge)
void psram_speed_gate_result(bool failed);

// Settings row and console
bool   psram_speed_choice(unsigned target_mhz); // record + register for the next boot; the caller restarts. False: unsupported
size_t psram_speed_value(char *out, size_t cap);   // "120 MHz failed, back at 80"
unsigned psram_speed_other(void);                  // the speed the row would switch to
void   psram_speed_cmd(char const *args);          // fw psram speed [80|120] [yes]
// A factory reset is under way: the restart that follows must come back at 80 MHz (STORE15 cleared, no re-arm)
void   psram_speed_no_rearm(void);
void   psram_speed_print(void);                    // the status lines of fw psram
