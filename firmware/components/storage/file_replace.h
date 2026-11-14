#pragma once
// Replacing a small file on the LittleFS drive so that a power cut at any step leaves the old content or the new one
// complete (#463). One helper instead of a hand-written write + rename in every service: write <path>.new, rename it
// over <path> (atomic on LittleFS). A cut before the rename leaves <path> as it was and a stale .new, which the next
// replace overwrites and the next recovered open removes.
//
// Not for the SD card: FAT's rename does not replace, and the card's users sit above this component. A path under
// /sd ("/sd" itself and "/sd/...", not "/sdx") is refused with EINVAL; so are an empty path and one over
// FILE_REPLACE_PATH_MAX (ENAMETOOLONG).

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define FILE_REPLACE_PATH_MAX 128 // longest <path>; the side name adds 4

// True: <path> holds buf. False: <path> still holds what it did, errno set.
bool lichen_file_replace(char const *path, void const *buf, size_t len);

// fopen(path, "r") after removing a stale <path>.new that a cut replace left. NULL with errno set when the stale file
// cannot be removed (the failure is not hidden as ENOENT) or the open fails. It writes (the unlink), so a replace and
// a recovered open of one path must be serialised by the caller.
FILE *lichen_file_open_recovered(char const *path);

// Once at boot, for a file an older firmware swapped with another side name (log.tmp, notify.tmp, notify.old): when
// <path> is missing the legacy file was the only good copy and is renamed into place, otherwise it is removed.
void lichen_file_adopt_legacy(char const *path, char const *legacy);
