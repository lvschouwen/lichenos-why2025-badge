#pragma once
// The manifest of an SD app (#521): /sd/lichen/apps/<id>/manifest, a small key=value text file. Pure (no ESP-IDF),
// host-tested in host_tests/lichen_manifest_test.c.
//
//   id=notes            required, equals the directory name (the app id rules of app_ns.h / fs_path.h)
//   name=Notes          required, 1..31 printable ASCII characters
//   api=1.0             required, MAJOR.MINOR of the app API the app was built for
//   entry=app.elf       optional, a file next to the manifest (default app.elf)
//   icon=icon.bin       optional, a file next to the manifest
//   caps=SD NET         optional, CAP_ names of lichen_caps.h separated by spaces or commas (not SYSTEM)
//   internal_ram_kib=8  optional budgets in KiB, like lichen_app_t (0 = none declared); stack_kib is for the task (#523)
//   psram_kib=64
//   stack_kib=4
//
// Lines end in LF or CRLF (the last needs none). Rule: a line starting with # is a comment and a blank line is
// ignored; a # anywhere else is part of the value. Every value is plain data: name is shown as text and is never used
// as a format string. A key may appear
// once; an unknown key, a line over LICHEN_MANIFEST_LINE_MAX or a malformed value refuses the whole file, so a typo is
// caught rather than ignored. Capabilities and budgets are shown and checked like a built-in app's, not enforced
// through the exports.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LICHEN_MANIFEST_LINE_MAX 127
#define LICHEN_MANIFEST_NAME_MAX 32 // with the NUL, also the longest entry and icon file name
#define LICHEN_MANIFEST_ID_MAX   14 // 13 + NUL (app_ns.h)

typedef struct {
    char     id[LICHEN_MANIFEST_ID_MAX];
    char     name[LICHEN_MANIFEST_NAME_MAX];
    char     entry[LICHEN_MANIFEST_NAME_MAX];
    char     icon[LICHEN_MANIFEST_NAME_MAX]; // "" = none
    uint16_t api_major;
    uint16_t api_minor;
    uint32_t caps;
    uint16_t internal_ram_kib;
    uint16_t psram_kib;
    uint16_t stack_kib;
} lichen_manifest_t;

typedef enum {
    LICHEN_MANIFEST_OK = 0,
    LICHEN_MANIFEST_ERR_ARG,       // NULL text or out
    LICHEN_MANIFEST_ERR_LINE,      // a line that is not key=value, or longer than LICHEN_MANIFEST_LINE_MAX
    LICHEN_MANIFEST_ERR_KEY,       // an unknown key
    LICHEN_MANIFEST_ERR_DUPLICATE, // a key twice
    LICHEN_MANIFEST_ERR_ID,        // a bad id, or one that is not the directory's name
    LICHEN_MANIFEST_ERR_VALUE,     // a bad name, file name, number or capability
    LICHEN_MANIFEST_ERR_VERSION,   // api is not MAJOR.MINOR
    LICHEN_MANIFEST_ERR_MISSING,   // id, name or api is absent
} lichen_manifest_err_t;

// Parses len bytes of text for the app in directory dir_id into *out (untouched unless OK). *err_line (may be NULL)
// gets the 1-based line of the failure, 0 when none or for a missing key.
lichen_manifest_err_t lichen_manifest_parse(char const *text, size_t len, char const *dir_id, lichen_manifest_t *out,
                                            unsigned *err_line);
// A short reason for the log and the console ("duplicate key", ...)
char const *lichen_manifest_err_text(lichen_manifest_err_t err);

// May this OS run an app built for app_major.app_minor? Same major and an app minor not above the OS's.
bool lichen_api_compatible(unsigned app_major, unsigned app_minor);

#ifdef __cplusplus
}
#endif
