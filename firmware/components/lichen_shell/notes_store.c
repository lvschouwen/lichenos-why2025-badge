#include "notes_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_CAP   160
#define READ_CAP   (NOTES_LINE_MAX + 64) // a line plus what notes_clean_line drops
#define NAME_TRIES 100

static bool path_join(char const *dir, char const *name, char const *suffix, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s%s", dir, name, suffix);
    return n > 0 && (size_t)n < cap;
}

static notes_result_t result_from_errno(int err) {
    return err == ENOSPC || err == EFBIG ? NOTES_ERR_SPACE : NOTES_ERR_IO;
}

static bool read_line(char const *path, char *line, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    char   raw[READ_CAP];
    size_t got = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    notes_clean_line(raw, got, line, cap);
    return line[0] != '\0';
}

static int by_name(void const *a, void const *b) {
    return notes_name_compare(((notes_item_t const *)a)->name, ((notes_item_t const *)b)->name);
}

notes_result_t notes_store_list(char const *dir, notes_item_t *out, size_t max, size_t *count, size_t *skipped) {
    *count   = 0;
    *skipped = 0;
    DIR *d   = opendir(dir);
    if (!d) {
        return NOTES_ERR_OPEN;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char path[PATH_CAP];
        if (!notes_name_ok(e->d_name) || !path_join(dir, e->d_name, "", path, sizeof(path))) {
            continue; // not ours: leftovers (.new), other files, hidden entries
        }
        if (*count >= max || !read_line(path, out[*count].line, sizeof(out[*count].line))) {
            (*skipped)++;
            continue;
        }
        memcpy(out[*count].name, e->d_name, strlen(e->d_name) + 1); // notes_name_ok: fits
        (*count)++;
    }
    closedir(d);
    qsort(out, *count, sizeof(*out), by_name);
    return NOTES_OK;
}

static notes_result_t write_file(char const *tmp, char const *path, char const *line) {
    FILE *f = fopen(tmp, "w");
    if (!f) {
        return result_from_errno(errno) == NOTES_ERR_SPACE ? NOTES_ERR_SPACE : NOTES_ERR_OPEN;
    }
    bool ok  = fputs(line, f) >= 0 && fputc('\n', f) != EOF;
    int  err = errno;
    ok       = (fclose(f) == 0) && ok;
    if (!ok) {
        err = errno ? errno : err;
        remove(tmp);
        return result_from_errno(err);
    }
    // FAT's rename does not replace: the finished temp file is there, so removing the old note first loses nothing
    remove(path);
    if (rename(tmp, path) != 0) {
        err = errno;
        remove(tmp);
        return result_from_errno(err);
    }
    return NOTES_OK;
}

notes_result_t notes_store_write(char const *dir, char const *name, char const *line) {
    char path[PATH_CAP], tmp[PATH_CAP];
    if (!notes_name_ok(name) || !line || !line[0] || !path_join(dir, name, "", path, sizeof(path)) ||
        !path_join(dir, name, ".new", tmp, sizeof(tmp))) {
        return NOTES_ERR_ARG;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return NOTES_ERR_GONE; // an edit of a note that was removed meanwhile does not bring it back
    }
    return write_file(tmp, path, line);
}

notes_result_t notes_store_add(char const *dir, char const *line, char *name_out, size_t cap) {
    char const *body;
    notes_parse(line, &body);
    if (!line[0] || !body[0]) {
        return NOTES_ERR_ARG;
    }
    for (unsigned n = 0; n < NAME_TRIES; n++) {
        char name[NOTES_NAME_MAX], path[PATH_CAP], tmp[PATH_CAP];
        struct stat st;
        if (!notes_file_name(body, n, name, sizeof(name)) || !path_join(dir, name, "", path, sizeof(path)) ||
            !path_join(dir, name, ".new", tmp, sizeof(tmp))) {
            return NOTES_ERR_ARG;
        }
        if (stat(path, &st) == 0) {
            continue;
        }
        notes_result_t r = write_file(tmp, path, line);
        if (r == NOTES_OK && name_out) {
            snprintf(name_out, cap, "%s", name);
        }
        return r;
    }
    return NOTES_ERR_SPACE; // 100 notes with the same first words
}

notes_result_t notes_store_delete(char const *dir, char const *name) {
    char path[PATH_CAP];
    if (!notes_name_ok(name) || !path_join(dir, name, "", path, sizeof(path))) {
        return NOTES_ERR_ARG;
    }
    if (remove(path) != 0) {
        return errno == ENOENT ? NOTES_ERR_GONE : NOTES_ERR_IO;
    }
    return NOTES_OK;
}

char const *notes_result_text(notes_result_t r) {
    switch (r) {
    case NOTES_OK:        return "Done.";
    case NOTES_ERR_ARG:   return "That note cannot be saved.";
    case NOTES_ERR_OPEN:  return "The notes folder could not be opened.";
    case NOTES_ERR_SPACE: return "The SD card is full.";
    case NOTES_ERR_IO:    return "The SD card could not be written.";
    case NOTES_ERR_GONE:  return "That note is gone.";
    }
    return "Failed.";
}
