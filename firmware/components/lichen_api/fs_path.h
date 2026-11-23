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

// Where an SD app's file call may go (#533). dir is the app's directory (lichen_fs_sd_dir: absolute, no trailing slash);
// path is what the app passed: relative to dir, or absolute and under it. The answer is the canonical path inside dir
// ("." and doubled slashes dropped), written whole into out, or false (out is "") for anything that could leave dir or
// that FAT would read differently from the rule: empty, ".." or a component ending in a dot or space (FatFs strips
// those), a backslash (FatFs takes it as a separator) or ':' (a drive), a sibling that only shares dir's prefix, an
// absolute path elsewhere, a path that does not fit out. Never truncates. with_root: dir itself is acceptable (mkdir:
// the answer is dir); without it the path must name something below dir (open, remove and rename never touch the folder).
bool fs_sandbox_resolve(char const *dir, char const *path, bool with_root, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
