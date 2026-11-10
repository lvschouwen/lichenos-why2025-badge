// "fw storage [dir]" (#299): free space on /storage and the size of each entry in dir (folders summed), so a full drive
// shows on the console instead of only in the Files app. Runs on the console task (small stack): one static path
// buffer for the whole walk, and the walk stops at MAX_DEPTH.
#include "storage.h"

#include "fw_console.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_DEPTH 4 // each level is an opendir on the console task's small stack: keep it shallow

static char s_path[256];

// Bytes under s_path (a file's size, or a folder's files summed); s_path is restored before it returns
static uint64_t tree_bytes(int depth) {
    struct stat st;
    if (stat(s_path, &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return (uint64_t)st.st_size;
    }
    if (depth >= MAX_DEPTH) {
        return 0;
    }
    DIR *dir = opendir(s_path);
    if (!dir) {
        return 0;
    }
    size_t         len   = strlen(s_path);
    uint64_t       total = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (snprintf(s_path + len, sizeof(s_path) - len, "/%s", e->d_name) < (int)(sizeof(s_path) - len)) {
            total += tree_bytes(depth + 1);
        }
        s_path[len] = 0;
    }
    closedir(dir);
    return total;
}

static bool dir_arg_ok(char const *dir) { // under /storage only: no absolute path, no way up
    return dir[0] != '/' && strstr(dir, "..") == NULL && strlen(dir) < 128;
}

static void cmd_storage(char const *args) {
    uint64_t free_b, total_b;
    if (!storage_mounted()) {
        console_reply("storage: not mounted");
        return;
    }
    bool known = storage_free_bytes(&free_b, &total_b); // unknown: a failed count, or nothing free; still list
    char arg[128];
    while (*args == ' ') {
        args++;
    }
    snprintf(arg, sizeof(arg), "%s", args);
    for (size_t n = strlen(arg); n > 0 && arg[n - 1] == ' ';) {
        arg[--n] = 0;
    }
    args = arg;
    if (!dir_arg_ok(args)) {
        console_reply("storage [dir] (a folder under " STORAGE_BASE ", e.g. \"c6\")");
        return;
    }
    snprintf(s_path, sizeof(s_path), "%s%s%s", STORAGE_BASE, args[0] ? "/" : "", args);
    if (known) {
        console_reply("storage: %llu bytes free of %llu (%llu%%) | listing %s (file bytes; LittleFS takes whole 4 KB "
                      "blocks)", (unsigned long long)free_b, (unsigned long long)total_b,
                      total_b ? (unsigned long long)(free_b * 100 / total_b) : 0ULL, s_path);
    } else {
        console_reply("storage: free space unknown (the count failed, or nothing is free) | listing %s", s_path);
    }
    DIR *dir = opendir(s_path);
    if (!dir) {
        console_reply("storage: %s is not a folder", s_path);
        return;
    }
    size_t         len = strlen(s_path);
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (snprintf(s_path + len, sizeof(s_path) - len, "/%s", e->d_name) >= (int)(sizeof(s_path) - len)) {
            s_path[len] = 0;
            continue;
        }
        bool is_dir = e->d_type == DT_DIR;
        console_reply("storage: %s%s %llu", e->d_name, is_dir ? "/" : "", (unsigned long long)tree_bytes(1));
        s_path[len] = 0;
    }
    closedir(dir);
}

void storage_console_register(void) {
    console_register("storage", "storage [dir] (free space on " STORAGE_BASE " and the bytes in each entry of dir, #299)",
                     cmd_storage);
}
