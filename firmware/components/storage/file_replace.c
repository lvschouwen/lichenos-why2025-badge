#include "file_replace.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char fresh[FILE_REPLACE_PATH_MAX + 8];
    char old[FILE_REPLACE_PATH_MAX + 8];
} sides_t;

static bool sides_of(char const *path, sides_t *s) {
    size_t n = strlen(path);
    if (n == 0 || n > FILE_REPLACE_PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(s->fresh, path, n);
    memcpy(s->fresh + n, ".new", 5);
    memcpy(s->old, path, n);
    memcpy(s->old + n, ".old", 5);
    return true;
}

static bool recover_sides(file_ops_t const *o, void *ctx, char const *path, sides_t const *s) {
    if (o->exists(ctx, path) || !o->exists(ctx, s->old)) {
        return true; // nothing half done (a .new beside <path> or on its own is an unfinished write)
    }
    // <path> was moved aside: the finished .new is the newer copy, the .old the previous one
    char const *from = o->exists(ctx, s->fresh) ? s->fresh : s->old;
    if (!o->rename(ctx, from, path)) {
        return false;
    }
    o->remove(ctx, s->old);
    return true;
}

bool file_recover_ops(file_ops_t const *o, void *ctx, char const *path, bool fat) {
    sides_t s;
    return !fat || !sides_of(path, &s) || recover_sides(o, ctx, path, &s);
}

static bool swap_fat(file_ops_t const *o, void *ctx, char const *path, sides_t const *s) {
    // From here .new is complete. A stale .old is older than <path> and would make the rename below fail.
    if (!o->remove(ctx, s->old)) {
        return false;
    }
    bool had = o->rename(ctx, path, s->old); // false also when there is no <path> yet
    if (!o->rename(ctx, s->fresh, path)) {
        return false; // do not undo: that would be another window; the recovery finds .new (or .old)
    }
    if (had) {
        o->remove(ctx, s->old); // best effort: a leftover is older than <path> and never preferred
    }
    return true;
}

bool file_replace_ops(file_ops_t const *o, void *ctx, char const *path, void const *buf, size_t len, bool fat) {
    sides_t s;
    if (!sides_of(path, &s)) {
        return false;
    }
    if (fat && !recover_sides(o, ctx, path, &s)) {
        return false; // a copy is still waiting: writing .new now would overwrite the one the recovery needs
    }
    if (!o->write(ctx, s.fresh, buf, len)) {
        int err = errno;
        o->remove(ctx, s.fresh);
        errno = err;
        return false;
    }
    bool ok = fat ? swap_fat(o, ctx, path, &s) : o->rename(ctx, s.fresh, path);
    if (!ok && !fat) {
        int err = errno;
        o->remove(ctx, s.fresh);
        errno = err;
    }
    return ok;
}

static bool posix_write(void *ctx, char const *path, void const *buf, size_t len) {
    (void)ctx;
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    setvbuf(f, NULL, _IONBF, 0);
    bool ok  = len == 0 || fwrite(buf, 1, len, f) == len;
    int  err = errno;
    ok       = fclose(f) == 0 && ok;
    if (!ok && !errno) {
        errno = err;
    }
    return ok;
}

static bool posix_rename(void *ctx, char const *from, char const *to) {
    (void)ctx;
    return rename(from, to) == 0;
}

static bool posix_remove(void *ctx, char const *path) {
    (void)ctx;
    return remove(path) == 0 || errno == ENOENT;
}

static bool posix_exists(void *ctx, char const *path) {
    (void)ctx;
    struct stat st;
    return stat(path, &st) == 0;
}

static file_ops_t const POSIX_OPS = {posix_write, posix_rename, posix_remove, posix_exists};

static bool is_fat(char const *path) {
    return strncmp(path, "/sd/", 4) == 0;
}

bool lichen_file_replace(char const *path, void const *buf, size_t len) {
    return file_replace_ops(&POSIX_OPS, NULL, path, buf, len, is_fat(path));
}

FILE *lichen_file_open_recovered(char const *path) {
    file_recover_ops(&POSIX_OPS, NULL, path, is_fat(path));
    return fopen(path, "r");
}
