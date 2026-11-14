// See lichen_sys_data_fs.h (#233)
#include "lichen_sys_data_fs.h"

#include "fs_path.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The drives (FAT, LittleFS) have no links, and the IDF's C library has no lstat; on the host the walk is tested with
// real links and must not follow them
static int entry_stat(char const *path, struct stat *st) {
#ifdef ESP_PLATFORM
    return stat(path, st);
#else
    return lstat(path, st);
#endif
}

bool lichen_sys_data_dir_path(char const *base, char const *id, char *out, size_t cap) {
    if (out && cap > 0) {
        out[0] = '\0';
    }
    if (!out || !fs_app_dir_path(base, id, out, cap)) {
        return false;
    }
    if (strlen(out) >= APP_DATA_PATH_MAX) { // the walk's path buffer is sized for it
        out[0] = '\0';
        return false;
    }
    return true;
}

// One walk, one path buffer: each level appends its entry's name and cuts it off again, so the stack holds a DIR and a
// few counters per level, however long the paths are
typedef struct {
    char          path[APP_DATA_PATH_MAX + 64];
    size_t        len;
    bool          remove;
    bool          failed; // a directory could not be read (measure) or something is left (clear)
    app_data_fs_t r;
} walk_t;

static void walk_dir(walk_t *w, int depth);

static bool enter_name(walk_t *w, char const *name, size_t *keep) {
    size_t n = strlen(name);
    *keep    = w->len;
    if (w->len + 1 + n >= sizeof(w->path)) {
        return false;
    }
    w->path[w->len] = '/';
    memcpy(w->path + w->len + 1, name, n + 1);
    w->len += 1 + n;
    return true;
}

static void leave_name(walk_t *w, size_t keep) {
    w->len          = keep;
    w->path[w->len] = '\0';
}

// A plain file counts its size and goes when removing; anything that is not a file or a directory (a link, a device)
// is never followed or counted: removing unlinks the entry itself, so a link out of the tree cannot be walked through
static void one_entry(walk_t *w, int depth) {
    struct stat st;
    if (entry_stat(w->path, &st) != 0) {
        w->r.skipped++;
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        if (depth >= APP_DATA_DEPTH_MAX) {
            w->r.skipped++;
            return;
        }
        walk_dir(w, depth + 1);
        if (w->remove) {
            if (rmdir(w->path) == 0) {
                w->r.dirs++;
            } else {
                w->r.skipped++;
            }
        } else {
            w->r.dirs++;
        }
        return;
    }
    if (w->remove) {
        if (unlink(w->path) == 0) {
            w->r.files++;
            w->r.bytes += S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
        } else {
            w->r.skipped++;
        }
    } else if (S_ISREG(st.st_mode)) {
        w->r.files++;
        w->r.bytes += (uint64_t)st.st_size;
    } else {
        w->r.skipped++;
    }
}

static void walk_dir(walk_t *w, int depth) {
    DIR *d = opendir(w->path);
    if (!d) {
        w->failed = true;
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        size_t keep;
        if (!enter_name(w, de->d_name, &keep)) {
            w->r.skipped++;
            continue;
        }
        one_entry(w, depth);
        leave_name(w, keep);
    }
    closedir(d);
}

static bool run(char const *dir, bool remove, app_data_fs_t *out) {
    app_data_fs_t none = {0};
    if (!out) {
        out = &none;
    }
    memset(out, 0, sizeof(*out));
    if (!dir || !dir[0] || strlen(dir) >= APP_DATA_PATH_MAX) {
        return false;
    }
    struct stat st;
    if (entry_stat(dir, &st) != 0) {
        return errno == ENOENT; // not there: nothing to count and nothing to clear
    }
    if (!S_ISDIR(st.st_mode)) {
        return false; // the app's directory is never a link or a file
    }
    walk_t  wk = {.remove = remove};
    walk_t *w  = &wk;
    strcpy(w->path, dir);
    w->len = strlen(dir);
    walk_dir(w, 0);
    *out = w->r;
    return !w->failed && (!remove || w->r.skipped == 0);
}

bool lichen_sys_data_dir_measure(char const *dir, app_data_fs_t *out) {
    return run(dir, false, out);
}

bool lichen_sys_data_dir_clear(char const *dir, app_data_fs_t *out) {
    return run(dir, true, out);
}
