#pragma once
// Where the notification history is kept on the storage drive: lichen_notify.c saves it, notify_cmd.c names it (#286).
#include "storage.h"

#define NOTIFY_DIR  STORAGE_BASE "/lichen"
#define NOTIFY_FILE NOTIFY_DIR "/notify.bin"
// The swap files of older firmware, adopted or removed once at load (lichen_file_adopt_legacy)
#define NOTIFY_LEGACY_TMP NOTIFY_DIR "/notify.tmp"
#define NOTIFY_LEGACY_OLD NOTIFY_DIR "/notify.old"
