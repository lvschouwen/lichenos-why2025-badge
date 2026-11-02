#include "files_ops.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Host tests run the same code on a temp directory: every file call goes through real_path(), which prefixes the
// path with files_test_root there (on the badge it is the path itself)
#ifdef FILES_HOST_TEST
extern char files_test_root[];
#endif

static char const *real_path(char const *path, char *buf, size_t cap) {
#ifdef FILES_HOST_TEST
    snprintf(buf, cap, "%s%s", files_test_root, path);
    return buf;
#else
    (void)buf;
    (void)cap;
    return path;
#endif
}

char const *files_result_text(files_result_t r) {
    switch (r) {
    case FILES_OK:            return "Done.";
    case FILES_CANCELLED:     return "Cancelled, nothing was left behind.";
    case FILES_ERR_ARG:       return "That name or path is not usable.";
    case FILES_ERR_PROTECTED: return "Protected: Lichen needs this, it stays read-only.";
    case FILES_ERR_EXISTS:    return "A file with that name is already there.";
    case FILES_ERR_NOT_FILE:  return "Only files can be copied, not folders.";
    case FILES_ERR_NOT_EMPTY: return "Only empty folders can be deleted.";
    case FILES_ERR_SPACE:     return "Not enough room on the target.";
    case FILES_ERR_OPEN:      return "Could not open it (is the card still in?).";
    case FILES_ERR_IO:        return "A read or write failed.";
    case FILES_ERR_NO_MEM:    return "Not enough memory.";
    }
    return "Failed.";
}

int files_list(char const *dir, files_entry_t *out, size_t max, size_t *count, size_t *skipped) {
    *count = *skipped = 0;
    if (!files_path_ok(dir)) {
        return EINVAL;
    }
    char rb[FILES_PATH_MAX + 64];
    DIR *d = opendir(real_path(dir, rb, sizeof(rb)));
    if (!d) {
        return errno ? errno : EIO;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char path[FILES_PATH_MAX];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        if (*count >= max || !files_path_join(dir, de->d_name, path, sizeof(path))) {
            (*skipped)++;
            continue;
        }
        struct stat st;
        files_entry_t *e = &out[*count];
        memset(e, 0, sizeof(*e));
        memcpy(e->name, de->d_name, strlen(de->d_name) + 1); // short enough: files_path_join took it
        if (stat(real_path(path, rb, sizeof(rb)), &st) == 0) {
            e->dir  = S_ISDIR(st.st_mode);
            e->size = e->dir ? 0 : (uint32_t)st.st_size;
        }
        (*count)++;
    }
    closedir(d);
    files_sort(out, *count);
    return 0;
}

files_result_t files_delete(char const *path) {
    if (!files_path_ok(path)) {
        return FILES_ERR_ARG;
    }
    if (files_protected(path)) {
        return FILES_ERR_PROTECTED;
    }
    struct stat st;
    char        rb[FILES_PATH_MAX + 64];
    char const *rp = real_path(path, rb, sizeof(rb));
    if (stat(rp, &st) != 0) {
        return FILES_ERR_OPEN;
    }
    if (S_ISDIR(st.st_mode)) {
        if (rmdir(rp) == 0) {
            return FILES_OK;
        }
        return errno == ENOTEMPTY || errno == EEXIST ? FILES_ERR_NOT_EMPTY : FILES_ERR_IO;
    }
    return unlink(rp) == 0 ? FILES_OK : FILES_ERR_IO;
}

files_result_t files_read_at(char const *path, uint32_t offset, uint8_t *buf, size_t cap, size_t *got, uint32_t *total) {
    *got = 0;
    *total = 0;
    if (!files_path_ok(path)) {
        return FILES_ERR_ARG;
    }
    if (files_protected(path)) {
        return FILES_ERR_PROTECTED;
    }
    char  rb[FILES_PATH_MAX + 64];
    FILE *f = fopen(real_path(path, rb, sizeof(rb)), "rb");
    if (!f) {
        return FILES_ERR_OPEN;
    }
    files_result_t r = FILES_OK;
    if (fseek(f, 0, SEEK_END) != 0) {
        r = FILES_ERR_IO;
    } else {
        long size = ftell(f);
        *total    = size > 0 ? (uint32_t)size : 0;
        if (size < 0 || fseek(f, (long)offset, SEEK_SET) != 0) {
            r = FILES_ERR_IO;
        } else {
            *got = fread(buf, 1, cap, f);
            if (*got < cap && ferror(f)) {
                r = FILES_ERR_IO;
            }
        }
    }
    fclose(f);
    return r;
}

// ---- copy ---------------------------------------------------------------------------------------------------------

static files_result_t copy_checks(char const *src, char const *dst, uint64_t free_bytes, uint32_t *size) {
    if (!files_path_ok(src) || !files_path_ok(dst) || strlen(dst) + 6 >= FILES_PATH_MAX) {
        return FILES_ERR_ARG;
    }
    if (files_protected(src) || files_protected(dst)) {
        return FILES_ERR_PROTECTED;
    }
    struct stat st;
    char        rb[FILES_PATH_MAX + 64];
    if (stat(real_path(src, rb, sizeof(rb)), &st) != 0) {
        return FILES_ERR_OPEN;
    }
    if (!S_ISREG(st.st_mode)) {
        return FILES_ERR_NOT_FILE;
    }
    *size = (uint32_t)st.st_size;
    struct stat existing;
    if (stat(real_path(dst, rb, sizeof(rb)), &existing) == 0) {
        return FILES_ERR_EXISTS;
    }
    return (uint64_t)st.st_size > free_bytes ? FILES_ERR_SPACE : FILES_OK;
}

// The data loop. Returns FILES_OK when every byte was written and flushed.
static files_result_t copy_data(FILE *in, FILE *out, uint8_t *buf, size_t cap, uint32_t total, files_progress_fn progress,
                                void *ctx) {
    uint32_t done = 0;
    for (;;) {
        size_t n = fread(buf, 1, cap, in);
        if (n == 0) {
            return ferror(in) ? FILES_ERR_IO : FILES_OK;
        }
        if (fwrite(buf, 1, n, out) != n) {
            return errno == ENOSPC ? FILES_ERR_SPACE : FILES_ERR_IO;
        }
        done += (uint32_t)n;
        if (progress && !progress(done, total, ctx)) {
            return FILES_CANCELLED;
        }
    }
}

files_result_t files_copy(char const *src, char const *dst, uint8_t *buf, size_t buf_cap, uint64_t free_bytes,
                          files_progress_fn progress, void *ctx) {
    uint32_t       total = 0;
    files_result_t r     = buf && buf_cap ? copy_checks(src, dst, free_bytes, &total) : FILES_ERR_NO_MEM;
    if (r != FILES_OK) {
        return r;
    }
    char part[FILES_PATH_MAX], rpart[FILES_PATH_MAX + 64], rsrc[FILES_PATH_MAX + 64], rdst[FILES_PATH_MAX + 64];
    snprintf(part, sizeof(part), "%s.part", dst);
    real_path(part, rpart, sizeof(rpart));
    struct stat st;
    if (stat(rpart, &st) == 0 && unlink(rpart) != 0) { // a leftover of an earlier failed copy
        return FILES_ERR_IO;
    }
    FILE *in  = fopen(real_path(src, rsrc, sizeof(rsrc)), "rb");
    FILE *out = in ? fopen(rpart, "wb") : NULL;
    if (!out) {
        if (in) {
            fclose(in);
        }
        return FILES_ERR_OPEN;
    }
    errno = 0;
    r = copy_data(in, out, buf, buf_cap, total, progress, ctx);
    fclose(in);
    if (fclose(out) != 0 && r == FILES_OK) {
        r = errno == ENOSPC ? FILES_ERR_SPACE : FILES_ERR_IO;
    }
    if (r == FILES_OK && rename(rpart, real_path(dst, rdst, sizeof(rdst))) != 0) {
        r = FILES_ERR_IO;
    }
    if (r != FILES_OK) {
        unlink(rpart);
    }
    return r;
}
