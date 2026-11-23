#pragma once

// The one call an SD-card app's main() makes (#523, API 2.1): it hands the OS the hooks of its screens. The OS registers
// the app from its manifest (lichen_manifest.h) and builds the lichen_app_t itself; every hook of that struct is an OS
// trampoline that enters the app's context and calls the hooks below only while the ELF is loaded, so nothing of the
// OS points into the app's code. The lifecycle (docs/app-api.md, "SD apps"):
//   - the tile is on Home from the card's mount, with no ELF loaded;
//   - opening the tile loads the ELF, runs main() once on a task of the app's own (stack_kib of the manifest), and
//     main() calls lichen_sd_app_hooks(); the OS then calls hooks.open on the LVGL task;
//   - when the app's last screen is closed the OS calls hooks.stop, waits for the app's tasks to end (main() returned,
//     every task it started returned), drops its timers and queued UI calls, and unloads the ELF.
// Everything below is for apps (L6) and the OS's loader alone.

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the app gives the OS. size is sizeof of the struct the app was built with: a later API adds members at the end, an
// older app passes a shorter struct and the OS treats what it does not cover as NULL.
typedef struct {
    uint32_t size;
    // Builds the app's screen (it is empty, styled and in the shell's stack); LVGL task, under the LVGL lock, in the app's
    // context, like lichen_app_t.open. Required.
    void (*open)(lv_obj_t *screen);
    // The app's last screen was closed, or the card went, or an update starts: end everything the app runs (its tasks must
    // return soon after, main() included), save what must survive. Called on the OS's loader task, in the app's context,
    // once per load; it may block for up to 5 s. True when the app stopped. NULL: nothing to stop but main() returning.
    bool (*stop)(void);
} lichen_sd_hooks_t;

// Called by main(), on the app's task, before it returns or goes on (inside the first 5 s of main()). False (nothing
// kept) when called outside a load, twice, or with no open hook. Any task of the app that is running for a load.
bool lichen_sd_app_hooks(lichen_sd_hooks_t const *hooks);

#ifdef __cplusplus
}
#endif
