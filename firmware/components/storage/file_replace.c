#include "file_replace.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool on_card(char const *path) {
    return strncmp(path, "/sd", 3) == 0 && (path[3] == '\0' || path[3] == '/');
}

// <path>.new into side; false (errno set) for a path the helper refuses
static bool side_of(char const *path, char side[FILE_REPLACE_PATH_MAX + 5]) {
    size_t n = strlen(path);
    if (n == 0 || on_card(path)) {
        errno = EINVAL;
        return false;
    }
    if (n > FILE_REPLACE_PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(side, path, n);
    memcpy(side + n, ".new", 5);
    return true;
}

// Unbuffered, one fwrite: no stdio buffer on the heap keeps a copy of a secret
static bool write_whole(char const *path, void const *buf, size_t len) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    setvbuf(f, NULL, _IONBF, 0);
    errno   = 0;
    bool ok = len == 0 || fwrite(buf, 1, len, f) == len;
    int  err = errno;
    ok       = fclose(f) == 0 && ok;
    if (!ok && errno == 0) {
        errno = err;
    }
    return ok;
}

bool lichen_file_replace(char const *path, void const *buf, size_t len) {
    char side[FILE_REPLACE_PATH_MAX + 5];
    if (!side_of(path, side)) {
        return false;
    }
    if (write_whole(side, buf, len) && rename(side, path) == 0) {
        return true;
    }
    int err = errno;
    unlink(side); // not remove(): that would take an empty directory too
    errno = err;
    return false;
}

FILE *lichen_file_open_recovered(char const *path) {
    char side[FILE_REPLACE_PATH_MAX + 5];
    if (!side_of(path, side)) {
        return NULL;
    }
    if (unlink(side) != 0 && errno != ENOENT) {
        return NULL;
    }
    return fopen(path, "r");
}

void lichen_file_adopt_legacy(char const *path, char const *legacy) {
    struct stat st;
    if (stat(legacy, &st) != 0) {
        return;
    }
    if (stat(path, &st) != 0 && errno == ENOENT) {
        rename(legacy, path);
    } else {
        unlink(legacy);
    }
}
