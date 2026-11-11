#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Per-boot PSRAM tuning report and dense check (#40). The IDF picks the MSPI phase and delayline on every boot and only
// logs a failure to the ROM console; here the choice is captured (linker wrap of the two select functions), read back
// from the MSPI registers, logged at WARN, and a dense 4 MB pattern check runs once. A bad boot is noted in the restart
// log ("fw crash"). The reboot-once answer to a bad boot is computed and logged but only carried out when
// PSRAM_GUARD_REBOOT_ON_BAD is 1 (off: the first round only collects data).

#ifndef PSRAM_GUARD_REBOOT_ON_BAD
#define PSRAM_GUARD_REBOOT_ON_BAD 0
#endif

// After the heaps are up: reads the tuning, runs the dense check, logs. Does not touch NVS (it may not be up yet).
void psram_boot_check(void);

// Once NVS is up (the guard task calls it ~3 s after boot): restart-log note and retry policy for a bad boot
void psram_boot_policy(void);

// The boot check (or the tuning) found this boot bad; why gets the reason ("" when good). Set before the shell starts,
// read-only afterwards: any task (#62's notice)
bool psram_boot_bad(char *why, size_t cap);

// Bad bytes the dense boot check found (both passes, and the same block checked again): PSRAM evidence for the speed
// switch (#353), unlike a fallen-back or short tuning window, which only marks the boot bad. Read-only after the check
size_t psram_boot_bad_bytes(void);

// "fw psram": tuning, registers, boot check, LDO lines
void psram_boot_print(void);

// "fw psram test <KB> dense": the same pattern + inverted check on demand, bad bytes per pass
void psram_boot_dense_test(unsigned kb);

// "fw psram dl <idx> yes" (#121): set delayline idx 0..30 live (DQS pins get the dqs delay, every other pin the data
// delay, as the IDF's tuning does), with the other core stalled for the switch. Nothing is stored: a reboot restores the
// IDF's choice. For sweeping the DMA window (`fw psram dma`) point by point without a reboot per point. Refused without
// the literal "yes", outside the passing window the boot tuning measured, and while the SD card is mounted.
void psram_boot_set_delayline(char const *idx_text);

// "fw psram dlboot <idx> yes | clear" (#121): arm a one-shot delayline for the NEXT boot. psram_boot_oneshot_apply runs
// once NVS is up (app_main, before storage and MeshCore): it erases the record first, then, only for an idx inside that
// boot's own tuning window, sets it and runs the dense check; the result is in "fw psram" and the boot log. A failed or
// unknown idx therefore never sticks: the boot after it is back on the IDF's choice.
void psram_boot_oneshot_arm(char const *args);
void psram_boot_oneshot_apply(void);

// "fw psram carrier" (#121): what the LP register (STORE15) held when PSRAM init read it; psram_speed.c owns the register
void psram_boot_carrier(char const *args);

// The tuning line ("phase 67.50 (run 4/4), delayline idx 15 ..."), for the Bench app (#96); returns the length
size_t psram_boot_tuning_text(char *out, size_t cap);

// The MSPI controller's live tuning registers: DQS0 phase id, DQS0 delay (90 degree tap), pin D's data delay. Plain
// register reads, any task (psram.*, #316)
void psram_boot_tuning_regs(uint8_t *phase_id, uint8_t *dqs, uint8_t *data);
// The delayline idx the boot tuning chose (the IDF's, before any one-shot or `fw psram dl`); false when not captured
bool psram_boot_dl_choice(uint8_t *idx);
