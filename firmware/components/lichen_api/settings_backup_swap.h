#pragma once
// The settings backup file's swap (#453). FAT has no atomic replace (rename refuses an existing target), so a save is
// a fixed sequence in which a complete, readable copy exists after every step, and a read picks the best copy that
// validates. Pure: the file operations come in through sbs_ops_t (settings_backup_io.c on the card, a fake in
// host_tests/settings_backup_swap_test.c). Shaped so it could be lifted into a shared replace helper later.
//
// Names, in the order a read prefers them:
//   SBS_MAIN  settings.txt  the backup
//   SBS_NEW   settings.new  a save in flight; complete once the write returned (a cut mid-write leaves a partial one)
//   SBS_OLD   settings.old  the previous backup, moved aside while the new one is renamed in
// Save: write NEW; MAIN -> OLD; NEW -> MAIN; remove OLD. A power cut or a failed call anywhere leaves a copy that the
// read accepts: MAIN until step 2, NEW or OLD between steps 2 and 3, MAIN after. A partial NEW fails validation and the
// read falls through to the copy that is intact.

#include <stdbool.h>
#include <stddef.h>

typedef enum { SBS_MAIN, SBS_NEW, SBS_OLD, SBS_COUNT } sbs_name_t;

typedef struct {
    bool (*write)(void *ctx, sbs_name_t name, char const *buf, size_t len); // create or truncate, write, close
    bool (*rename)(void *ctx, sbs_name_t from, sbs_name_t to);              // false when `to` exists (FAT)
    bool (*remove)(void *ctx, sbs_name_t name);                             // an absent file counts as removed
} sbs_ops_t;

// Runs the sequence. True: MAIN holds buf. False: the earlier copies are intact or NEW holds buf (the read finds it).
bool sbs_save(sbs_ops_t const *ops, void *ctx, char const *buf, size_t len);

typedef enum { SBS_ABSENT, SBS_INVALID, SBS_VALID } sbs_state_t;
typedef sbs_state_t (*sbs_probe_fn)(void *ctx, sbs_name_t name); // reads the file and validates its content

// The copy to load: the first VALID of MAIN, NEW, OLD (probed in that order, stopping at the first). A MAIN that exists but
// is invalid ends the search: only an absent MAIN falls back to the copies a cut save leaves. SBS_COUNT when none
// is; *saw_file then tells whether any existed (a refused file, not a missing one). Content is never trusted unprobed.
sbs_name_t sbs_choose(sbs_probe_fn probe, void *ctx, bool *saw_file);
