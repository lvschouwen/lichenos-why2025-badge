#pragma once

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

// "fw psram": tuning, registers, boot check, LDO lines
void psram_boot_print(void);

// "fw psram test <KB> dense": the same pattern + inverted check on demand, bad bytes per pass
void psram_boot_dense_test(unsigned kb);
