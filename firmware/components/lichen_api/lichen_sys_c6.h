#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The C6 link and the LoRa radio's numbers, the shell's and the OS watchdogs' side (#284): the status bar, About,
// Settings and the stress run read them here, never from c6link.h or lora_app.h. System surface: tools/layers.conf
// forbids lichen_sys_* in L6. Every call is quick and never waits on a lock that may be held for long.

// The C6 link is up: brightness and the radio need it. Any task.
bool lichen_sys_c6_up(void);
// The C6 is being recovered or is gone (#14): the state in words ("recovering 2/3", "down (no answer)"); false while it
// is up. Any task.
bool lichen_sys_c6_trouble(char *out, size_t cap);
// C6 outages the badge came back from (#14); reason gets the last recovery in words. Only the sys_notices task calls it
// (the status is too big for a stack). Not on the LVGL task; one caller at a time (it uses a shared static).
uint32_t lichen_sys_c6_restarts(char *reason, size_t cap);
// The LoRa radio runs and the C6 link is up. Any task.
bool lichen_sys_lora_up(void);
// LoRa packets received and sent so far, both only going up (the status bar's TX/RX chevrons, polled every 100 ms,
// #29). Any task.
void lichen_sys_lora_counts(uint32_t *rx, uint32_t *tx);
