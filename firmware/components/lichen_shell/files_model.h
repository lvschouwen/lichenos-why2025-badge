#pragma once
// Files app (#71): the pure rules. Paths, what is protected, how a listing sorts, how a text file is cut into screens.
// ESP-free, host-tested in host_tests/files_model_test.c; files_ops.c does the file work, app_files.c the screens.

#include "lichen_sys_storage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FILES_PATH_MAX 256
#define FILES_NAME_MAX 128 // longer names are left out of the listing (counted as skipped)
#define FILES_STORAGE  LICHEN_SYS_STORAGE_BASE
#define FILES_SD       LICHEN_SYS_SD_BASE

typedef struct {
    char     name[FILES_NAME_MAX];
    uint32_t size; // FAT: a file is under 4 GB
    bool     dir;
} files_entry_t;

// A usable absolute path: "/storage" or "/sd", then plain components. No "..", ".", empty or over-long components, no
// trailing slash, no control characters.
bool files_path_ok(char const *path);
// A plain file or directory name: not empty, not "." or "..", no '/', no control characters, fits FILES_NAME_MAX
bool files_name_ok(char const *name);
// Also fine on a FAT card, which is stricter than files_name_ok (#443). FatFs (ff.c create_name) refuses " * : < > ? | \,
// DEL and invalid UTF-8 with FR_INVALID_NAME. A trailing space or dot it silently snips, so the card's name would differ
// from the source's (and "a" and "a." would collide): Lichen refuses those too, as its own rule.
bool files_name_ok_fat(char const *name);
// dir + "/" + name; false (out untouched) for a bad name or when it does not fit
bool files_path_join(char const *dir, char const *name, char *out, size_t cap);
// "/storage/a/b" -> "/storage/a"; "/storage" -> "" (the volume list). False for a path that is not ok or already ""
bool files_path_parent(char const *path, char *out, size_t cap);
// The volume root a path is on ("/storage" or "/sd"), NULL for anything else
char const *files_volume_of(char const *path);
// The other volume's root, for a copy
char const *files_other_volume(char const *volume);

// Never deleted, overwritten, copied from or into. Deny by default: on /storage every directory and everything below
// one (MeshCore, the C6 images, crash log, Lichen state, every app's directory), the root files lora.toml and
// ota_token.txt (and any "ota_*" or "lora*" root file), and the volume roots; on /sd the whole /lichen tree. What is
// left is the user's: other plain files at the /storage root and everything else on the card. A path that is not ok
// counts as protected. Case-insensitive and blind to trailing dots and spaces and to 8.3 "~1" aliases in the first
// component, the way FAT matches names.
bool files_protected(char const *path);
// files_protected plus: a directory directly under /storage is protected whatever its name (the path alone can't say
// whether a root entry is a file or a directory)
bool files_protected_entry(char const *path, bool is_dir);
// Not even shown read-only: the update token, the card's /lichen data and the apps' secret roots (the MeshCore identity
// and keys)
bool files_secret(char const *path);
// An app's directory under /storage (lichen_app_t.storage_root): protected, and secret when asked. Repeats are fine.
// False: bad name or the list (16) is full. files_app_roots_clear is for tests.
bool files_add_app_root(char const *name, bool secret);
void files_app_roots_clear(void);

// Directories first, then names without regard to case, then exactly
void files_sort(files_entry_t *entries, size_t count);

// ---- text paging --------------------------------------------------------------------------------------------------

// Width of one character (UTF-8 bytes, n long) in whatever unit `cols` is given in, e.g. pixels
typedef int (*files_width_fn)(void *ctx, char const *utf8, size_t n);

// Cuts buf[0..len) into at most `lines` lines of at most `cols` width for one screen: 1 per character, or the width
// from `measure` (NULL: 1). A line breaks at its last blank so words stay whole, and inside a word only when the word
// alone is longer than a line (#144); a blank at the break is dropped. Newlines end a line (CR is
// dropped), tabs become spaces, other control bytes and invalid UTF-8 show as '.' / '?'. Valid UTF-8 passes through
// and is never split. Writes out (NUL-terminated) and sets *consumed to the bytes the screen used (> 0 whenever len > 0
// and at_eof or len is at least 4 bytes more than a screen needs). at_eof false: a sequence cut off at the end of the
// buffer is left for the next read. *binary is set when more than a tenth of the bytes were control bytes.
typedef struct {
    uint8_t const *buf;
    size_t         len;
    bool           at_eof;
    int            cols;
    int            lines;
    files_width_fn measure;
    void          *measure_ctx;
} files_text_in_t;

typedef struct {
    char   *out;
    size_t  cap;
    size_t *consumed;
    bool   *binary;
} files_text_out_t;

size_t files_text_page(files_text_in_t const *in, files_text_out_t const *out);

#ifdef __cplusplus
}
#endif
