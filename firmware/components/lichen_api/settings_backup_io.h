#pragma once
// Settings backup (#169) on the SD card: /sd/lichen/settings.txt in the format of settings_backup.h. The NVS reads and
// writes and the file work; the rules are settings_backup.c's. None of these runs on the LVGL task (NVS commits and SD
// I/O), and the load is two steps so the person sees what changes before anything is written.

#include "settings_backup.h"

#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_BACKUP_PATH "/sd/lichen/settings.txt"

typedef enum {
    SBK_OK,
    SBK_NO_CARD,
    SBK_NO_FILE,
    SBK_IO,      // the card or NVS failed
    SBK_INVALID, // load: the file is refused (see the sb_result_t)
    SBK_NOTHING, // load: every value in the file is what the badge has already
    SBK_MEMORY,
} sbk_status_t;

// Writes every allow-listed setting as it is now
sbk_status_t settings_backup_save(void);

typedef struct {
    sb_result_t result;  // SBK_INVALID: why
    size_t      changed; // values that differ from the badge now (held for settings_backup_apply)
    char        summary[200]; // "3 settings will change: ..."
} settings_backup_plan_t;

// Reads and checks the file and holds the changes; nothing is written to NVS
sbk_status_t settings_backup_plan(settings_backup_plan_t *plan);
// Writes the held changes to NVS; *applied counts the ones stored. SBK_IO when any failed (the others stay written)
sbk_status_t settings_backup_apply(size_t *applied);
// Drops the held changes (the person said no, or left the screen). Any task, but not while a plan or apply runs.
void settings_backup_discard(void);
