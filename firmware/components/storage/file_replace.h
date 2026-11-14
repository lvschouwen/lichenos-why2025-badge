#pragma once
// Replacing a small file so that a power cut at any step leaves the old content or the new one complete (#463).
// One helper instead of a hand-written write + rename in every service. Pure over file_ops_t (file_replace_test.c
// cuts it after every step); the plain-POSIX binding is lichen_file_replace below.
//
// LittleFS (/storage): write <path>.new, rename it over <path> (atomic there).
// FAT (/sd): rename does not replace, so: write <path>.new; remove <path>.old; <path> -> <path>.old; <path>.new ->
// <path>; remove <path>.old. A cut between the last renames leaves no <path>, but a complete .new beside a .old: the
// recovery promotes it. A .new with no .old beside it is an unfinished first write and is never used.

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define FILE_REPLACE_PATH_MAX 96 // longest <path>; the side names add 4

typedef struct {
    bool (*write)(void *ctx, char const *path, void const *buf, size_t len); // create or truncate, write, close
    bool (*rename)(void *ctx, char const *from, char const *to);             // false when `to` exists on FAT
    bool (*remove)(void *ctx, char const *path);                             // an absent file counts as removed
    bool (*exists)(void *ctx, char const *path);
} file_ops_t;

// True: <path> holds buf. False: <path> still holds what it did (or a complete copy the recovery finds), errno set.
bool file_replace_ops(file_ops_t const *ops, void *ctx, char const *path, void const *buf, size_t len, bool fat);
// Finishes a replace that a cut left half done. False when a copy exists that could not be put back.
bool file_recover_ops(file_ops_t const *ops, void *ctx, char const *path, bool fat);

// The same over stdio and POSIX; "/sd/..." is FAT, everything else LittleFS. The file is written unbuffered in one
// fwrite (no stdio buffer holds a copy of a secret).
bool lichen_file_replace(char const *path, void const *buf, size_t len);
// fopen(path, "r") after the recovery: the file a cut replace left, or NULL (errno ENOENT) when there is none.
FILE *lichen_file_open_recovered(char const *path);
