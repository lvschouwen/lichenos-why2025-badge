#pragma once
// Which files "fw sd cat" (#378) may read, without ESP headers: host-tested in host_tests/sd_path_policy_test.c.
// sd_console.c is the glue (it also checks that the path is a regular file).
//
// A path is taken exactly as typed, never rewritten: anything a file system could read as a different name is refused
// rather than normalised, so the name checked against the deny list is the name that gets opened.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_PATH_MAX 192 // bytes of a path, without the NUL; longer ones are refused

typedef enum {
    SD_PATH_OK,
    SD_PATH_BAD,      // not absolute, or an empty, ".", ".." or dot-trick component, a control or non-ASCII byte, ':'...
    SD_PATH_ROOT,     // not below /sd or /storage (the volume roots themselves included)
    SD_PATH_TOO_LONG, // over SD_PATH_MAX
    SD_PATH_DENIED,   // the deny list (any depth): the update token, MeshCore's identity, keys and prefs, key files, NVS
                      // backups
} sd_path_verdict_t;

// The deny list wins over everything else. Case-insensitive, the way FAT matches names.
sd_path_verdict_t sd_path_check(char const *path);
// One word for a refusal: "bad path", "outside /sd and /storage", "path too long", "denied"
char const *sd_path_verdict_text(sd_path_verdict_t v);

#ifdef __cplusplus
}
#endif
