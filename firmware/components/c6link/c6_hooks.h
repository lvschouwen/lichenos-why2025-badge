#pragma once

#include <stdbool.h>
#include <stdint.h>

// What c6link, an L2 driver, needs from the layers above it (#223, design in #216): upward calls go only through
// callbacks the higher layer registers, never through its headers. Given once at boot, before c6link_start (main.c
// gives the boot hooks, lichen_net's wifi_service_init the net hooks). Any member may be NULL, and so may the whole
// table: an unset hook does the default named at its member, so a hook that fires before it is registered (or one
// that never is) is defined. Pure and host-tested (host_tests/c6_hooks_test.c).

// The boot screen's "C6" step (lichen_boot). Default: the step is not shown. And ota's trial state.
typedef struct {
    void (*begin)(void);
    void (*progress)(int percent); // the flasher, 0-100 per image
    void (*end)(bool ok, char const *note);
    // The running image is still on trial (ota_image_trial_pending, #285): a reset now rolls the update back, so the
    // boot runs the full C6 check and the supervisor never offers the fallback restart. Default: true (on trial)
    bool (*image_on_trial)(void);
} c6link_boot_hooks_t;

// The Wi-Fi service (lichen_net), which rides on the link.
typedef struct {
    // The boot attach worked: start Wi-Fi. Default: no Wi-Fi this boot (c6_hooks_attached returns false)
    void (*attached)(void);
    // The boot attach failed: end the boot steps that need the C6. Default: nothing
    void (*no_link)(void);
    // The supervisor (c6sup) re-attaches: take the driver down before esp_hosted_deinit, bring it up after the attach.
    // Wait at most wait_ms; false when it did not finish in time. Default: true at once (no driver to wait for)
    bool (*driver_down)(uint32_t wait_ms);
    bool (*driver_up)(uint32_t wait_ms);
    // The Wi-Fi task's stack high-water mark in bytes, for the recovery log. Default: 0
    unsigned (*stack_free)(void);
} c6link_net_hooks_t;

// The note c6link passes to the boot hook's end, NUL included (main.c asserts lichen_boot's BOOT_REASON_LEN holds it)
#define C6LINK_STEP_NOTE_LEN 48

// Copy the table (NULL puts every default back). Not safe once c6link_start has run: the c6sup task reads them.
void c6link_set_boot_hooks(c6link_boot_hooks_t const *hooks);
void c6link_set_net_hooks(c6link_net_hooks_t const *hooks);
