#pragma once

#include <stdbool.h>
#include <stdint.h>

// Update mode (#76): while a firmware image is being received everything not needed for it is stopped, because the
// badge restarts afterwards anyway and the transfer needs the internal RAM. Pure: says what to do per event and owns
// the restart decision; the update screen does the doing. A zeroed update_state_t is "normal".

typedef enum { UM_NORMAL, UM_UPDATING } update_mode_t;

typedef enum {
    UM_ACT_NONE          = 0,
    UM_ACT_STOP_APPS     = 1, // every app's stop_for_update, plus the stop hook
    UM_ACT_CLOSE_SCREENS = 2, // every screen but the update screen
    UM_ACT_REBOOT        = 4, // after the failure message has been shown
    UM_ACT_REFUSE        = 8, // a file push while updating: the OTA server answers 409
} update_act_t;

// How long a prepare may wait for the firmware to follow before it counts as a failed update
#define UPDATE_PREPARE_TIMEOUT_MS 60000

typedef struct {
    update_mode_t mode;
    bool          prepared;    // prepared, no firmware begin yet
    bool          reboot;      // restart once the failure message has been shown
    int64_t       prepared_ms;
} update_state_t;

// The sender asks the badge to get ready (POST /prepare): enters update mode like a firmware begin would. Asked again
// (a retry), nothing more to stop; it also takes back a pending restart.
unsigned update_mode_on_prepare(update_state_t *s, int64_t now_ms);
// A transfer starts. Only a firmware image enters update mode (a file push leaves everything running). Once updating, a
// file push is refused (UM_ACT_REFUSE, state untouched) and a firmware begin takes back a pending restart.
unsigned update_mode_on_begin(update_state_t *s, bool firmware);
// The transfer ended. A failed firmware update in update mode reboots the badge (everything is stopped); a good one is
// rebooted by the sender as usual and does not cancel a restart already decided. A file end while updating is refused.
unsigned update_mode_on_end(update_state_t *s, bool firmware, bool ok);
// Called regularly: a prepare that no firmware followed in UPDATE_PREPARE_TIMEOUT_MS is a failed update (REBOOT, once)
unsigned update_mode_tick(update_state_t *s, int64_t now_ms);
// An installed image that nobody rebooted: restart anyway (the badge has no home to go back to)
void update_mode_on_restart_stall(update_state_t *s);
bool update_mode_file_allowed(update_state_t const *s);
// A restart has been decided and the failure message (if any) has been shown
bool update_mode_restart_due(update_state_t const *s, bool message_done);

// What the OS itself stops after the apps' stop_for_update (the LoRa radio when every app stopped, the C6 link's update
// state), set once from main.c. NULL = nothing.
void update_mode_set_stop_hook(void (*hook)(bool apps_stopped));
void update_mode_run_stop_hook(bool apps_stopped);
