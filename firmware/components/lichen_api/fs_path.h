#pragma once
// Where an app's files live (#84 T6, #60). Pure, host-tested in host_tests/fs_path_test.c; lichen_fs.c is the glue.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// "<base>/<id>" for an id that follows the app id rules (app_ns.h: 1..13 of a-z 0-9 _), so the directory is always
// one plain component below base: no "..", no "/", nothing a card's FAT would mangle. False (out untouched) for any
// other id, a base that is not absolute, or a buffer too small. Also false for an id that names a directory the OS
// keeps on the storage drive ("c6": the C6 image, /storage/c6; "lichen": the notification store): no app gets it, so
// an app's file shim can never format or write into it (#283, moved here from fs_shim's format check).
bool fs_app_dir_path(char const *base, char const *id, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
