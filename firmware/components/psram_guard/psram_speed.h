#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "psram_speed_logic.h"

// The PSRAM speed switch (#121, #353), the part that touches NVS, the LP register and the restart; the rules are in
// psram_speed_logic.c. Needs the patched IDF (tools/idf-patches/0003, CONFIG_SPIRAM_SPEED_RUNTIME_SELECT): without it
// everything here reports 80 MHz, not supported, and never restarts.

// True when the IDF can run 120 MHz
bool psram_speed_supported(void);
// The speed PSRAM init chose this boot, MHz
unsigned psram_speed_running(void);
// The speed the record asks for (80 or 120; 80 again after a fall on evidence), MHz. One byte read: any task, no lock
unsigned psram_speed_want(void);
// What LP_STORE15 held when PSRAM init read (and cleared) it, for the boot log and "fw psram carrier"
uint32_t psram_speed_carrier_raw(void);

// app_main, once NVS is up: reads the record (120 MHz when nothing is saved, #353), applies the rules of
// psram_speed_logic.h (bad bytes in this boot's check at 120 = back at 80; 120 wanted at a boot at 80 = arm and restart
// once, or after a crash at the next clean restart), registers the shutdown hook that keeps a clean restart at 120 MHz.
// image_on_trial answers whether the running image may still be on trial (ota_image_trial_maybe, #285): no restart
// for the speed while it may be, as that would roll the image back. NULL counts as on trial. dump_fresh answers whether
// the core dump is new (crash_dump_fresh): a reason-7 reset is a crash only then. NULL: never.
void psram_speed_boot(bool (*image_on_trial)(void), bool (*dump_fresh)(void));

// Guard task, every ~100 ms (works once a second): judges reported evidence, runs the PSRAM heap check once a minute at
// 120 MHz, starts the arm / leave that had to wait for the image to be valid, clears the arm count of a settled boot.
// Quick: the NVS writes and restarts run on a job task, not here.
void psram_speed_tick(void);

// PSRAM evidence seen on any task (a canary flip, a failed DMA gate round): at 120 MHz the tick records the fall and
// restarts at 80. The first report of a boot is the one recorded. Any task, no lock, no NVS.
void psram_speed_report(psram_fail_kind_t kind);
// A DMA gate round's PSRAM verdict (psram_dma_evidence: bad PSRAM bytes, not card or DMA errors): evidence at 120 MHz
// (console: say so on the console first)
void psram_speed_gate_result(bool bad_bytes, bool console);
// The badge fell back to 80 MHz on evidence and stays there until a speed is chosen; why gets the notice text
bool psram_speed_fell(char *why, size_t cap);

// Settings row and console
bool   psram_speed_choice(unsigned target_mhz); // record + register for the next boot; the caller restarts. False: unsupported
size_t psram_speed_value(char *out, size_t cap);   // "120 MHz failed, back at 80"
unsigned psram_speed_other(void);                  // the speed the row would switch to
void   psram_speed_cmd(char const *args);          // fw psram speed [80|120] [yes]
// A factory reset is under way: the restart that follows comes back at 80 MHz (STORE15 cleared, no re-arm); the erased
// record then arms 120 as on a first boot
void   psram_speed_no_rearm(void);
void   psram_speed_print(void);                    // the status lines of fw psram (speed, last failure)
// A copy of the record (want, failed, arms, the last fall), for the psram.fail_* registry keys; any task
void   psram_speed_record(psram_speed_rec_t *out);
