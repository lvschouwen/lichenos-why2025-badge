#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Between c6link.c (boot attach, Wi-Fi) and c6_sup.c (the C6 supervisor, #14), not for other components

// c6link.c
void c6link_set_link(bool up);
bool c6link_updating(void);
bool c6link_image_is_trial(void);
// GPIO13 (the C6 GPIO9 boot strap): 1 boots the application at the next EN reset, 0 the ROM download mode
void hold_c6_boot(int level);
// Ask the c6wifi task to take the driver down (up=false) or bring it up again, and wait at most wait_ms for it
bool c6link_wifi_request(bool up, uint32_t wait_ms);
unsigned c6link_wifi_stack_free(void); // the c6wifi task's stack high-water mark (bytes)
// The next boot checks the C6 fully (NVS c6_verify), the stored bundle key stays
bool c6link_set_verify_flag(void);

// c6_sup.c
void c6_sup_start(void);    // before the boot attach: the watch, its NVS flag, the queue and the c6sup task
// After a successful boot attach: listen to ESP-Hosted's events. Recovery starts only once main.c has given the hooks
// (c6link_set_hooks), so a C6 lost before then is not recovered with half the hooks missing.
void c6_sup_attached(void);
// "fw c6 reset", "reset rom", "reattach"; false for any other argument
bool c6_sup_command(char const *args);
void c6_sup_watch_line(char *out, size_t cap); // the `fw c6` line
