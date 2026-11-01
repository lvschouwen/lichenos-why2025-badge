#pragma once
// Settings > SD card (#99), without ESP headers: what the page says and which rows act, per card state and running
// job. Host-tested in host_tests/sd_settings_model_test.c; sd_screen.c is the LVGL glue.

#include "sd_policy.h"

#include <stdbool.h>
#include <stddef.h>

// The one job the page may have running (mount, unmount or format on a short-lived task)
typedef enum { SDS_IDLE, SDS_MOUNTING, SDS_UNMOUNTING, SDS_FORMATTING } sds_job_t;

// The status line: "Mounted: SDHC 29.7 GB, 28.1 GB free", "No card found", "Not FAT (exFAT or blank)", "Error: ...",
// "Formatting..." while a job runs
void sds_status(sd_info_t const *info, sds_job_t job, char *out, size_t cap);

typedef struct {
    char const *mount_label;  // "Unmount" for a mounted card, else "Mount"
    char const *mount_value;  // the row's right-hand text
    bool        mount_enabled;
    char const *format_value;
    bool        format_enabled; // only for a card that answered: mounted, not FAT or failing
} sds_actions_t;

sds_actions_t sds_actions(sd_state_t state, sds_job_t job);

// What Enter on the Mount/Unmount row starts
sds_job_t sds_mount_job(sd_state_t state);

// The line under the rows once a job is over: ok is what the sd_* call returned (unmount: the card is no longer
// mounted), after is sd_info() right after it
void sds_result(sds_job_t job, bool ok, sd_info_t const *after, char *out, size_t cap);
